#include "progression/game/Service.h"

#include "progression/game/Actors.h"
#include "progression/game/Forms.h"
#include "progression/game/Learning.h"
#include "progression/game/Log.h"
#include "progression/game/PerkTrees.h"
#include "progression/game/PerkView.h"
#include "progression/game/Rules.h"
#include "progression/game/SpellView.h"
#include "progression/game/Tomes.h"

#include <algorithm>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace fp::game
{
namespace
{

// 60 m, in the engine's units: an archer on a ledge, a mage at the back.
constexpr float kNearby = 4200.0f;

// Guards g_state against the pages' reads. The game thread holds it for
// the whole of anything that changes state; a frame of the panel waits.
std::mutex g_mutex;

struct State
{
    std::vector<Companion> companions;
    std::vector<CompanionView> views;
    Settings settings;
    int playerLevel{1};
    bool inGame{false};
    Rules rules; // as the views last read them, for the pages
} g_state;

// The companions the views and the learning hooks were last published for,
// by runtime id: one loaded but missing (a reference that did not resolve
// at the load) is published on the next tick.
std::unordered_set<RE::FormID> g_published;

// Companions released since levelling was turned off, or since the load:
// while it is off, everyone not in here still carries something of ours.
// Under g_mutex.
std::unordered_set<FormKey, FormKeyHash> g_releasedDone;

void OnGameThread(std::function<void()> work)
{
    if (auto *tasks = SKSE::GetTaskInterface())
        tasks->AddTask(std::move(work));
}

Companion *Find(const FormKey &key)
{
    const auto it = std::find_if(g_state.companions.begin(), g_state.companions.end(),
                                 [&](const Companion &c) { return c.key == key; });
    return it == g_state.companions.end() ? nullptr : &*it;
}

RE::Actor *ActorOf(const FormKey &key)
{
    return Lookup<RE::Actor>(key);
}

bool IsHere(RE::Actor *actor)
{
    return actor && actor->Is3DLoaded() && DistanceToPlayer(actor) <= kNearby * 3.0f;
}

void Hud(const std::string &text)
{
    RE::SendHUDMessage::ShowHUDMessage(text.c_str());
}

void Refuse(const std::string &why)
{
    log::ui.info("refused: {}", why);
    Hud(why);
}

// The same, not shown: for the skill page's clicks, which ask first and say
// no with a sound of their own.
void RefuseQuietly(const std::string &why)
{
    log::ui.info("refused: {}", why);
}

// Perks on their record that were not bought here: theirs, or another mod's.
std::unordered_set<FormKey, FormKeyHash> OnRecord(const Companion &c, RE::Actor *actor)
{
    auto perks = BasePerks(actor);
    for (const LearnedPerk &p : c.perks)
        perks.erase(p.form);
    return perks;
}

// The spells taught to a companion that this load order has.
std::vector<RE::SpellItem *> TaughtSpells(const Companion &c)
{
    std::vector<RE::SpellItem *> out;
    for (const TaughtSpell &t : c.spells)
        if (auto *spell = Lookup<RE::SpellItem>(t.spell))
            out.push_back(spell);
    return out;
}

// Every companion's perks and spells as the engine is to see them
// (progression/game/PerkView.h, progression/game/SpellView.h): their record's, less the set-aside,
// plus the bought and the taught -- published whole after anything that
// could change it. A bought perk that is also on the record needs no adding;
// one this load order lacks is kept in the ledger and said once.
void PublishViews()
{
    static std::unordered_set<FormKey, FormKeyHash> missing;
    std::unordered_map<RE::FormID, perkview::Diff> diffs;
    std::unordered_map<RE::FormID, spellview::Diff> spellDiffs;
    if (g_state.settings.released)
    {
        perkview::Publish({}); // everyone as their record has them
        spellview::Publish({});
        learning::Publish({});
        g_published.clear();
        return;
    }
    std::vector<RE::FormID> learners;
    for (const Companion &c : g_state.companions)
    {
        RE::Actor *actor = ActorOf(c.key);
        if (!actor)
            continue;
        const auto record = BasePerks(actor);
        perkview::Diff diff;
        const auto resolve = [&](const FormKey &form) {
            RE::BGSPerk *perk = PerkOf(form);
            if (!perk && missing.insert(form).second)
                log::perks.warn("{}: {} is not in this load order; kept in the ledger, not applied", c.name,
                                ToString(form));
            return perk;
        };
        for (const FormKey &form : c.setAside)
            if (RE::BGSPerk *perk = resolve(form))
                diff.removed.push_back(perk);
        for (const LearnedPerk &p : c.perks)
            if (!record.contains(p.form))
                if (RE::BGSPerk *perk = resolve(p.form))
                    diff.added.push_back(perk);
        diffs.emplace(actor->GetFormID(), std::move(diff));

        spellview::Diff spells;
        for (const SpellAside &s : c.spellsSetAside)
            spells.removed.push_back(Lookup<RE::SpellItem>(s.spell));
        spells.added = TaughtSpells(c);
        spellDiffs.emplace(actor->GetFormID(), std::move(spells));
        learners.push_back(actor->GetFormID());
    }
    perkview::Publish(std::move(diffs));
    spellview::Publish(std::move(spellDiffs));
    learning::Publish(learners);
    g_published = {learners.begin(), learners.end()};
}

// A companion's level now, and the points it leaves them (progression/core/Companion.h).
LevelProgress LevelOf(const Companion &c, RE::Actor *actor, const Rules &r)
{
    return Progress(c, actor->GetLevel(), g_state.playerLevel, r);
}

int PerkPointsOf(const Companion &c, RE::Actor *actor, int level)
{
    return PerkPoints(level, HeldRanks(Graph(), HoldingsOf(c, OnRecord(c, actor))));
}

int AttributePointsOf(const Companion &c, RE::Actor *actor, int level, const Rules &r)
{
    return AttributePoints(c, level, OwnAttributePoints(BaseAttributes(actor), RaceStart(actor), r));
}

int FloorOf(RE::Actor *actor, Skill skill, const Rules &r)
{
    return SkillFloor(r, RaceSkillBonus(actor, skill));
}

// A rise in their level -- from what they learned, or from the engine
// levelling them with the player -- is a level-up, said with what it
// brings. The first reading, at enrolment, only sets where they start.
void NoteLevel(Companion &c, RE::Actor *actor, const Rules &r)
{
    const int level = LevelOf(c, actor, r).level;
    if (level <= c.level)
        return;
    const bool first = c.level == 0;
    c.level = level;
    if (first)
        return;
    log::growth.info("{} reached level {}", c.name, level);
    if (g_state.settings.notifyLevels)
    {
        const std::string what =
            ToAssign(PerkPointsOf(c, actor, level), AttributePointsOf(c, actor, level, r), c.learning.pool);
        Hud(what.empty() ? fmt::format("{} reached level {}.", c.name, level)
                         : fmt::format("{} reached level {}: {} to assign.", c.name, level, what));
    }
}

// Everything of ours off a companion, levelling off: assigned points
// withdrawn, perks back to their record's through the engine's rank change,
// and the abilities of bought perks dropped directly as well, in case the
// save kept one (dev/ENGINE_PERKS.md). The ledger is untouched, so turning
// levelling on puts it all back. The views are already empty (PublishViews).
void Release(Companion &c, RE::Actor *actor)
{
    if (g_releasedDone.contains(c.key))
        return;
    const Delta back = Withdrawal(c);
    if (!back.Empty())
    {
        ApplyPoints(actor, back);
        MarkApplied(c, back);
    }
    const auto record = BasePerks(actor);
    for (const LearnedPerk &p : c.perks)
        if (!record.contains(p.form))
            DropPerkAbilities(actor, PerkOf(p.form));
    perkview::Reconcile(actor);
    // Known through the view, which is empty now: out of their hands too,
    // so a save made next does not keep one there. Without the hooks (VR)
    // they were added to the actor, and stay: turning levelling on could not
    // add them back.
    if (spellview::Installed())
        for (RE::SpellItem *spell : TaughtSpells(c))
            spellview::Withdraw(actor, spell);
    g_releasedDone.insert(c.key);
    log::party.info("{}: released; points withdrawn, perks and spells as their record has them", c.name);
}

// The assigned points, perks and spells onto the actor: whatever the ledger
// says minus whatever is there already. Repeating it does nothing. With
// levelling off, the reverse.
void Reconcile(Companion &c, RE::Actor *actor)
{
    if (!actor)
        return;
    if (g_state.settings.released)
    {
        Release(c, actor);
        // Taught spells withdrawn by the release stay out of hand until the
        // fight that may still list them is over.
        spellview::Reconcile(actor);
        return;
    }
    const Delta pending = Pending(c, BaseSkills(actor), ReadRules().skillCap);
    if (!pending.Empty())
    {
        ApplyPoints(actor, pending);
        MarkApplied(c, pending);
        log::growth.debug("{}: assigned points applied to the actor", c.name);
    }
    perkview::Reconcile(actor);
    spellview::Reconcile(actor);
}

Companion &EnrollActor(RE::Actor *actor, const FormKey &key)
{
    Companion c = fp::Enroll(key, NameOf(actor));
    NoteLevel(c, actor, ReadRules());
    log::party.info("{} ({}) enrolled at level {}", c.name, ToString(key), c.level);
    g_state.companions.push_back(std::move(c));
    return g_state.companions.back();
}

void RebuildViews()
{
    const Rules r = ReadRules();
    g_state.rules = r;
    std::vector<CompanionView> views;
    for (const Companion &c : g_state.companions)
    {
        const auto previous = std::find_if(g_state.views.begin(), g_state.views.end(),
                                           [&](const CompanionView &v) { return v.key == c.key; });
        CompanionView v;
        v.key = c.key;
        RE::Actor *actor = ActorOf(c.key);
        v.actor = actor ? actor->GetFormID() : 0;
        if (actor && IsHere(actor))
        {
            v.read = true;
            v.loaded = true;
            v.level = actor->GetLevel();
            v.progress = LevelOf(c, actor, r);
            v.perkPoints = PerkPointsOf(c, actor, v.progress.level);
            v.attributePoints = AttributePointsOf(c, actor, v.progress.level, r);
            v.base = BaseSkills(actor);
            for (const Skill skill : AllSkills())
            {
                const std::size_t i = Index(skill);
                v.floors[i] = FloorOf(actor, skill, r);
                if (const auto usage = ReadSkillUsage(skill))
                    v.nextLevel[i] = SkillThreshold(r, *usage, v.base[i] + c.learning.skills[i]);
            }
            v.attributes = BaseAttributes(actor);
            v.onRecord = OnRecord(c, actor);
        }
        else if (previous != g_state.views.end())
        {
            // Away: the sheet as it was last read, marked as not here.
            v = *previous;
            v.loaded = false;
        }
        if (actor)
        {
            v.following = IsFollower(actor) && !IsWaiting(actor);
            v.waiting = IsFollower(actor) && IsWaiting(actor);
        }
        views.push_back(std::move(v));
    }
    g_state.views = std::move(views);
}

// Every action from a page: on the game thread, under the lock, with the
// views rebuilt after so the page shows the result on its next frame.
void Act(std::function<void()> work)
{
    OnGameThread([work = std::move(work)] {
        std::scoped_lock lock(g_mutex);
        if (!g_state.inGame)
            return;
        work();
        PublishViews();
        RebuildViews();
    });
}

// Why a page's buttons cannot change a companion now, in the panel's
// words; none when they can.
std::optional<std::string> CannotChange(const Companion &c, const CompanionView &v)
{
    if (g_state.settings.released)
        return "Leveling is off: turn it on in Follower Tactics' Settings.";
    if (!v.loaded)
        return c.name + " must be with you for this.";
    return std::nullopt;
}

// The companion and their actor, when they are here to be changed.
std::pair<Companion *, RE::Actor *> Present(const FormKey &key, std::string_view doing)
{
    Companion *c = Find(key);
    if (!c)
        return {nullptr, nullptr};
    if (g_state.settings.released)
    {
        Refuse("Leveling is off: turn it on in Follower Tactics' Settings first.");
        return {nullptr, nullptr};
    }
    RE::Actor *actor = ActorOf(key);
    if (!IsHere(actor))
    {
        Refuse(fmt::format("{} must be with you to {}.", c->name, doing));
        return {nullptr, nullptr};
    }
    return {c, actor};
}

} // namespace

// --- actions ---------------------------------------------------------------------

void AssignSkillPoint(const FormKey &key, Skill skill, int delta)
{
    Act([key = key, skill, delta] {
        auto [c, actor] = Present(key, "train");
        if (!c)
            return;
        const Rules r = ReadRules();
        const PerSkill<int> base = BaseSkills(actor);
        const AssignCheck check = CheckSkill(*c, skill, delta, base, FloorOf(actor, skill, r), Graph(),
                                             HoldingsOf(*c, OnRecord(*c, actor)), r);
        if (check.block == AssignBlock::PerkNeedsIt)
        {
            Refuse(
                fmt::format("{} needs that much {}: unlearn it first, or reset the skill.", check.perk, Name(skill)));
            return;
        }
        if (check.block != AssignBlock::None)
            return;
        fp::AssignSkill(*c, skill, delta, base[Index(skill)], r);
        Reconcile(*c, actor);
    });
}

void AssignAttributePoint(const FormKey &key, Attribute attribute, int delta)
{
    Act([key = key, attribute, delta] {
        auto [c, actor] = Present(key, "train");
        if (!c)
            return;
        const Rules r = ReadRules();
        const int available = AttributePointsOf(*c, actor, LevelOf(*c, actor, r).level, r);
        if (CheckAttribute(*c, attribute, delta, available) != AssignBlock::None)
            return;
        fp::AssignAttribute(*c, attribute, delta, r.attributePerLevel);
        Reconcile(*c, actor);
    });
}

namespace
{

// A rank bought, the next of `nodeId`'s: on the game thread, under the lock.
// The skill page's, which says it with a sound: nothing is shown.
void LearnNode(const FormKey &key, int nodeId)
{
    auto [c, actor] = Present(key, "learn a perk");
    if (!c)
        return;
    const PerkGraph &graph = Graph();
    if (nodeId < 0 || static_cast<std::size_t>(nodeId) >= graph.Size())
        return;
    const PerkNode &node = graph.Node(nodeId);
    const Holdings holdings = HoldingsOf(*c, OnRecord(*c, actor));
    const PerSkill<int> skills = Effective(*c, BaseSkills(actor));
    const int points = PerkPointsOf(*c, actor, LevelOf(*c, actor, ReadRules()).level);
    const PerkStatus status = Status({graph, holdings, skills, points}, nodeId);
    if (status.block != PerkBlock::None)
    {
        RefuseQuietly(fmt::format("{} cannot learn {} now.", c->name, node.name));
        return;
    }
    const int rank = status.held;
    const FormKey &form = node.ranks[static_cast<std::size_t>(rank)].form;
    RE::BGSPerk *perk = PerkOf(form);
    if (!perk)
    {
        RefuseQuietly(fmt::format("{} is not in this load order.", node.name));
        return;
    }
    fp::Learn(*c, node, rank);
    PublishViews();
    perkview::Reconcile(actor);
    // Through the engine's own HasPerk, which is our ForEachPerk.
    if (!actor->HasPerk(perk))
        log::perks.warn("{}: learned {}, but the engine's HasPerk says no", c->name, node.name);
    log::perks.info("{} learned {} ({}/{}), {}", c->name, node.name, rank + 1, node.ranks.size(), ToString(form));
}

} // namespace

void LearnPerkByForm(std::uint32_t actorId, std::uint32_t perkForm)
{
    Act([actorId, perkForm] {
        const auto actorKey = KeyOf(RE::TESForm::LookupByID<RE::Actor>(actorId));
        const auto perkKey = KeyOf(RE::TESForm::LookupByID<RE::BGSPerk>(perkForm));
        if (!actorKey || !perkKey)
            return;
        if (const auto node = Graph().Find(*perkKey))
            LearnNode(*actorKey, node->first);
    });
}

std::optional<LevelProgress> LevelFor(RE::FormID actor)
{
    if (actor == 0)
        return std::nullopt;
    std::scoped_lock lock(g_mutex);
    for (const CompanionView &v : g_state.views)
        if (v.actor == actor && v.read && Find(v.key))
            return v.progress;
    return std::nullopt;
}

std::optional<SkillControls> ControlsFor(RE::FormID actor, int actorValue)
{
    const auto skill = SkillFromActorValue(actorValue);
    if (!skill || actor == 0)
        return std::nullopt;
    std::scoped_lock lock(g_mutex);
    for (const CompanionView &v : g_state.views)
    {
        const Companion *found = v.actor == actor && v.read ? Find(v.key) : nullptr;
        if (!found)
            continue;
        const Companion &c = *found;
        const std::size_t k = Index(*skill);
        SkillControls out;
        out.companion = c.key;
        out.skill = *skill;
        out.base = v.base[k];
        out.learned = c.learning.skills[k];
        out.level = out.base + out.learned;
        out.perkPoints = v.perkPoints;
        out.buttons = ButtonsFor(c, *skill, v.base, v.floors[k], Graph(), HoldingsOf(c, v.onRecord), g_state.rules);
        if (const auto why = CannotChange(c, v))
        {
            out.buttons.canLower = out.buttons.canRaise = out.buttons.canResetPerks = false;
            out.buttons.lower = out.buttons.lowest = out.buttons.raise = out.buttons.highest = out.buttons.resetPerks =
                *why;
        }
        out.active = !CannotChange(c, v);
        return out;
    }
    return std::nullopt;
}

namespace
{

// The top rank of `nodeId` bought here, given back: on the game thread,
// under the lock. The skill page's, as LearnNode.
void UnlearnNode(const FormKey &key, int nodeId)
{
    auto [c, actor] = Present(key, "unlearn a perk");
    if (!c)
        return;
    const PerkGraph &graph = Graph();
    if (nodeId < 0 || static_cast<std::size_t>(nodeId) >= graph.Size())
        return;
    const PerkNode &node = graph.Node(nodeId);
    const Holdings holdings = HoldingsOf(*c, OnRecord(*c, actor));
    const PerSkill<int> skills = Effective(*c, BaseSkills(actor));
    const int points = PerkPointsOf(*c, actor, LevelOf(*c, actor, ReadRules()).level);
    const PerkStatus status = Status({graph, holdings, skills, points}, nodeId);
    if (!status.canUnlearn)
    {
        RefuseQuietly(fmt::format("{} cannot unlearn {}: something else needs it.", c->name, node.name));
        return;
    }
    const FormKey &form = node.ranks[static_cast<std::size_t>(status.held - 1)].form;
    fp::Unlearn(*c, form);
    PublishViews();
    perkview::Reconcile(actor);
    log::perks.info("{} unlearned {} (rank {})", c->name, node.name, status.held);
}

} // namespace

void UnlearnPerkByForm(std::uint32_t actorId, std::uint32_t perkForm)
{
    Act([actorId, perkForm] {
        const auto actorKey = KeyOf(RE::TESForm::LookupByID<RE::Actor>(actorId));
        const auto node = NodeOfPerk(perkForm);
        if (actorKey && node)
            UnlearnNode(*actorKey, *node);
    });
}

std::optional<PerkControls> PerkControlsFor(RE::FormID actor, std::uint32_t perk)
{
    const auto nodeId = NodeOfPerk(perk);
    if (!nodeId || actor == 0)
        return std::nullopt;
    std::scoped_lock lock(g_mutex);
    for (const CompanionView &v : g_state.views)
    {
        const Companion *c = v.actor == actor && v.read ? Find(v.key) : nullptr;
        if (!c)
            continue;
        PerkControls out;
        if (g_state.settings.released || !v.loaded)
            return out;
        const Holdings holdings = HoldingsOf(*c, v.onRecord);
        const PerSkill<int> skills = Effective(*c, v.base);
        const PerkStatus status = Status({Graph(), holdings, skills, v.perkPoints}, *nodeId);
        out.canLearn = status.block == PerkBlock::None;
        out.canUnlearn = status.canUnlearn;
        return out;
    }
    return std::nullopt;
}

void SetAsidePerk(const FormKey &key, int nodeId)
{
    Act([key = key, nodeId] {
        auto [c, actor] = Present(key, "set a perk aside");
        if (!c)
            return;
        const PerkGraph &graph = Graph();
        if (nodeId < 0 || static_cast<std::size_t>(nodeId) >= graph.Size())
            return;
        const PerkNode &node = graph.Node(nodeId);
        const auto onRecord = OnRecord(*c, actor);
        std::vector<FormKey> forms;
        for (const PerkRank &rank : node.ranks)
            if (onRecord.contains(rank.form))
                forms.push_back(rank.form);
        if (forms.empty())
            return;
        const Holdings holdings = HoldingsOf(*c, onRecord);
        const PerSkill<int> skills = Effective(*c, BaseSkills(actor));
        const PerkRules rules{graph, holdings, skills, PerkPointsOf(*c, actor, LevelOf(*c, actor, ReadRules()).level)};
        if (const auto broken = WouldBreak(rules, forms); !broken.empty())
        {
            Refuse(fmt::format("{} cannot set {} aside: {} needs it.", c->name, node.name,
                               graph.Node(broken.front()).name));
            return;
        }
        fp::SetAside(*c, node, onRecord);
        PublishViews();
        perkview::Reconcile(actor);
        log::perks.info("{} set {} aside", c->name, node.name);
        Hud(fmt::format("{} set {} aside.", c->name, node.name));
    });
}

void RestorePerk(const FormKey &key, int nodeId)
{
    Act([key = key, nodeId] {
        auto [c, actor] = Present(key, "take a perk up again");
        if (!c)
            return;
        const PerkGraph &graph = Graph();
        if (nodeId < 0 || static_cast<std::size_t>(nodeId) >= graph.Size())
            return;
        const PerkNode &node = graph.Node(nodeId);
        if (!fp::Restore(*c, node))
            return;
        PublishViews();
        perkview::Reconcile(actor);
        log::perks.info("{} took {} up again", c->name, node.name);
        Hud(fmt::format("{} took {} up again.", c->name, node.name));
    });
}

void LearnFromTome(std::uint32_t actorId, std::uint32_t bookId)
{
    Act([actorId, bookId] {
        const auto key = KeyOf(RE::TESForm::LookupByID<RE::Actor>(actorId));
        auto *book = RE::TESForm::LookupByID<RE::TESObjectBOOK>(bookId);
        RE::SpellItem *spell = TomeSpell(book);
        if (!key || !spell)
            return;
        auto [c, actor] = Present(*key, "learn a spell");
        if (!c)
            return;
        if (!spellview::Installed())
        {
            RefuseQuietly("the spell hooks are not installed: nothing can be taught");
            return;
        }
        if (CarriedCount(actor, book) <= 0)
        {
            RefuseQuietly(fmt::format("{} no longer carries {}", c->name, NameOf(book)));
            return;
        }
        // Known through the view (progression/game/SpellView.h), never added
        // to the actor. HasSpell is the engine's own walk through it, so
        // this asks what the engine will answer; checked again before the
        // tome goes, and on failure the ledger is put back as it was.
        const SpellFacts facts = FactsOf(spell);
        const auto taughtBefore = c->spells;
        const auto asideBefore = c->spellsSetAside;
        const TomeRead read = ReadTome(*c, facts, actor->HasSpell(spell));
        if (read == TomeRead::Known)
        {
            RefuseQuietly(fmt::format("{} already knows {}", c->name, facts.name));
            return;
        }
        PublishViews();
        if (!actor->HasSpell(spell))
        {
            log::spells.warn("{}: {} did not take (the engine's HasSpell says no); the tome is kept", c->name,
                             facts.name);
            c->spells = taughtBefore;
            c->spellsSetAside = asideBefore;
            PublishViews();
            return;
        }
        actor->RemoveItem(book, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        log::spells.info("{} {} {} from {}", c->name, read == TomeRead::Restored ? "took up again" : "learned",
                         facts.name, NameOf(book));
    });
}

void ForgetSpellByForm(std::uint32_t actorId, std::uint32_t spellId)
{
    Act([actorId, spellId] {
        const auto key = KeyOf(RE::TESForm::LookupByID<RE::Actor>(actorId));
        auto *spell = RE::TESForm::LookupByID<RE::SpellItem>(spellId);
        if (!key || !spell || spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell)
            return;
        auto [c, actor] = Present(*key, "forget a spell");
        if (!c)
            return;
        if (!spellview::Installed())
        {
            RefuseQuietly("the spell hooks are not installed: nothing can be forgotten");
            return;
        }
        const SpellFacts facts = FactsOf(spell);
        const SpellForgotten forgotten = ForgetSpell(*c, facts, actor->HasSpell(spell));
        if (forgotten == SpellForgotten::NotKnown)
            return;
        PublishViews();
        // Out of their hands and their voice now; a fight's inventory,
        // gathered before, still lists a taught one, so CheckCast refuses
        // it until the fight is over (spellview::Withdraw).
        if (forgotten == SpellForgotten::Forgotten)
            spellview::Withdraw(actor, spell);
        spellview::Reconcile(actor);
        if (actor->HasSpell(spell))
            log::spells.warn("{}: forgot {}, but the engine's HasSpell still says yes", c->name, facts.name);
        log::spells.info("{} forgot {} ({})", c->name, facts.name,
                         forgotten == SpellForgotten::Forgotten ? "taught here" : "theirs, set aside");
    });
}

std::optional<SpellControls> SpellControlsFor(RE::FormID actor)
{
    if (actor == 0)
        return std::nullopt;
    std::scoped_lock lock(g_mutex);
    for (const CompanionView &v : g_state.views)
    {
        const Companion *c = v.actor == actor && v.read ? Find(v.key) : nullptr;
        if (!c)
            continue;
        SpellControls out;
        if (const auto why = CannotChange(*c, v))
            out.why = *why;
        else if (!spellview::Installed())
            out.why = "The spell hooks are not installed: see the log.";
        else
            out.active = true;
        return out;
    }
    return std::nullopt;
}

namespace
{

void TurnOff()
{
    if (g_state.settings.released)
        return;
    g_state.settings.released = true;
    g_releasedDone.clear();
    PublishViews();
    std::size_t here = 0;
    for (Companion &c : g_state.companions)
        if (RE::Actor *actor = ActorOf(c.key); actor && actor->Is3DLoaded())
        {
            Release(c, actor);
            ++here;
        }
    const std::size_t away = g_state.companions.size() - here;
    log::party.info("leveling off: {} companion(s) released, {} not near", here, away);
    Hud(away == 0 ? std::string("Leveling is off: your companions are as their records have them.")
                  : fmt::format("Leveling is off: {} companion(s) released; {} more when they are near.", here, away));
}

void TurnOn()
{
    if (!g_state.settings.released)
        return;
    g_state.settings.released = false;
    g_releasedDone.clear();
    PublishViews();
    for (Companion &c : g_state.companions)
        if (RE::Actor *actor = ActorOf(c.key); actor && actor->Is3DLoaded())
            Reconcile(c, actor);
    log::party.info("leveling on: learned skills, points, perks and spells back on the companions here");
    Hud("Leveling is on: your companions' skills, perks and spells are back.");
}

} // namespace

LevellingState Levelling()
{
    std::scoped_lock lock(g_mutex);
    LevellingState out;
    out.inGame = g_state.inGame;
    out.on = !g_state.settings.released;
    if (!out.on)
        for (const Companion &c : g_state.companions)
            if (!g_releasedDone.contains(c.key))
                out.stillHeld.push_back(c.name);
    return out;
}

void SetLevelling(bool on)
{
    Act([on] {
        if (on)
            TurnOn();
        else
            TurnOff();
    });
}

void AssignSkillAll(const FormKey &key, Skill skill, int direction)
{
    Act([key = key, skill, direction] {
        auto [c, actor] = Present(key, "change their skills");
        if (!c)
            return;
        const Rules r = ReadRules();
        const int moved = fp::AssignSkillAll(*c, skill, direction, BaseSkills(actor), FloorOf(actor, skill, r), Graph(),
                                             HoldingsOf(*c, OnRecord(*c, actor)), r);
        if (moved == 0)
            return;
        Reconcile(*c, actor);
        log::growth.info("{}: {} {} by {}", c->name, Name(skill), direction < 0 ? "lowered" : "raised", moved);
    });
}

void ResetPerks(const FormKey &key, Skill skill)
{
    Act([key = key, skill] {
        auto [c, actor] = Present(key, "reset their perks");
        if (!c)
            return;
        const auto unlearned = fp::ResetPerks(*c, skill, Graph());
        if (unlearned.empty())
            return;
        PublishViews();
        perkview::Reconcile(actor);
        std::string names;
        for (const std::string &name : unlearned)
            names += (names.empty() ? "" : ", ") + name;
        log::perks.info("{}: {} perks reset: {}", c->name, Name(skill), names);
    });
}

// --- the game thread -------------------------------------------------------------------

void Tick()
{
    auto *player = RE::PlayerCharacter::GetSingleton();
    std::scoped_lock lock(g_mutex);
    if (!player || !g_state.inGame)
        return;
    g_state.playerLevel = player->GetLevel();

    // Who is here: the new enrolled.
    for (RE::Actor *actor : LoadedFollowers())
    {
        const auto key = KeyOf(actor);
        if (key && !Find(*key) && g_state.settings.autoEnroll && !g_state.settings.released && IsUniqueNpc(actor))
        {
            EnrollActor(actor, *key);
            PublishViews();
        }
    }

    // What they have onto every enrolled companion who is loaded, follower
    // or not: after a load, a dismissed companion's are put back as soon as
    // they are near, not when they are recruited again.
    bool unpublished = false;
    for (Companion &c : g_state.companions)
        if (RE::Actor *actor = ActorOf(c.key); actor && actor->Is3DLoaded())
        {
            unpublished = unpublished || !g_published.contains(actor->GetFormID());
            Reconcile(c, actor);
        }
    if (unpublished && !g_state.settings.released)
        PublishViews();
}

void RefreshViews()
{
    std::scoped_lock lock(g_mutex);
    if (g_state.inGame)
        RebuildViews();
}

void OnSkillUse(RE::FormID id, Skill skill, float points)
{
    std::scoped_lock lock(g_mutex);
    if (!g_state.inGame || g_state.settings.released)
        return;
    auto *actor = RE::TESForm::LookupByID<RE::Actor>(id);
    const auto key = KeyOf(actor);
    Companion *c = key ? Find(*key) : nullptr;
    if (!c)
        return;
    const auto usage = ReadSkillUsage(skill);
    if (!usage)
        return;
    const Rules r = ReadRules();
    const Practice practice = Practise(*c, skill, points, BaseSkills(actor)[Index(skill)], *usage, r);
    if (practice.skillUps == 0)
        return;
    log::growth.info("{}'s {} increased to {}", c->name, Name(skill), practice.reached);
    if (g_state.settings.notifySkills)
        Hud(fmt::format("{}'s {} increased to {}.", c->name, Name(skill), practice.reached));
    NoteLevel(*c, actor, r);
    Reconcile(*c, actor);
}

void OnPlayerLevelUp(int level)
{
    std::scoped_lock lock(g_mutex);
    if (!g_state.inGame)
        return;
    g_state.playerLevel = level;
    if (g_state.settings.released)
        return;
    // The engine's levels for followers who scale with the player move now,
    // and so does the cap on what learning can reach: either is a level-up.
    const Rules r = ReadRules();
    for (Companion &c : g_state.companions)
        if (RE::Actor *actor = ActorOf(c.key); actor && actor->Is3DLoaded())
            NoteLevel(c, actor, r);
}

void OnGameStarted()
{
    auto *player = RE::PlayerCharacter::GetSingleton();
    std::scoped_lock lock(g_mutex);
    g_state.inGame = true;
    g_state.playerLevel = player ? player->GetLevel() : 1;
    PublishViews();
}

void OnGameLeft()
{
    std::scoped_lock lock(g_mutex);
    g_state.inGame = false;
}

void BeforeLoad()
{
    // Nothing of ours is on a base record to take off. The views go, so an
    // actor rebuilt during the load is built as its record has it until the
    // save's ledger is read and published again.
    perkview::Forget();
    spellview::Forget();
    learning::Forget();
}

// --- the co-save ------------------------------------------------------------------------------

std::vector<CoSaveRecord> SaveRecords()
{
    std::scoped_lock lock(g_mutex);
    return PackCoSave(g_state.companions, g_state.settings);
}

void LoadRecords(const std::vector<CoSaveRecord> &records)
{
    CoSaveContents contents = UnpackCoSave(records);
    for (const std::string &note : contents.notes)
        log::save.warn("{}", note);
    std::scoped_lock lock(g_mutex);
    g_state.companions = std::move(contents.companions);
    g_state.settings = contents.settings.value_or(Settings{});
    g_state.views.clear();
    g_releasedDone.clear();
    // Before the save's actors are built, where the load allows: an actor
    // whose process is built after this has its view from the start.
    PublishViews();
    log::save.info("the save holds {} companion(s)", g_state.companions.size());
}

void Revert()
{
    std::scoped_lock lock(g_mutex);
    perkview::Forget();
    spellview::Forget();
    learning::Forget();
    g_published.clear();
    g_releasedDone.clear();
    g_state = State{};
}

} // namespace fp::game
