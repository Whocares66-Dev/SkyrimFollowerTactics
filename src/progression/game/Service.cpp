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
#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace fp::game
{
namespace
{

// 60 m, in the engine's units: an archer on a ledge, a mage at the back.
constexpr float kNearby = 4200.0f;

// Guards g_state against the panel's copies. The game thread holds it for
// the whole of anything that changes state; a frame of the panel waits.
std::mutex g_mutex;

struct State
{
    std::vector<Companion> companions;
    std::vector<CompanionView> views;
    std::vector<Candidate> candidates;
    Settings settings;
    int playerLevel{1};
    bool inGame{false};
    std::string refused;
    Rules rules; // as the views last read them, for the panel
} g_state;

std::atomic<std::uint64_t> g_version{1};
std::atomic<bool> g_panelShown{false};

// Set by the panel whenever it draws one of our pages; the tick rebuilds
// the views only when something has been looking at them.
std::atomic<bool> g_drawn{false};
// Who was following at the last tick: the test button's "everyone".
std::unordered_set<FormKey, FormKeyHash> g_following;

// The companions the views and the learning hooks were last published for,
// by runtime id: one loaded but missing (a reference that did not resolve
// at the load) is published on the next tick.
std::unordered_set<RE::FormID> g_published;

// Companions released since levelling was turned off, or since the load:
// while it is off, everyone not in here still carries something of ours.
// Under g_mutex.
std::unordered_set<FormKey, FormKeyHash> g_releasedDone;

void Changed() noexcept
{
    g_version.fetch_add(1, std::memory_order_relaxed);
}

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

void Refuse(std::string why)
{
    log::ui.info("refused: {}", why);
    Hud(why);
    g_state.refused = std::move(why);
}

// The same, not shown: for the skill page's clicks, which ask first and say
// no with a sound of their own.
void RefuseQuietly(std::string why)
{
    log::ui.info("refused: {}", why);
    g_state.refused = std::move(why);
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
    auto *player = RE::PlayerCharacter::GetSingleton();
    // The player's tomes once, priced per companion below; the rules once.
    const std::vector<Tome> tomes = TomesCarried(player);
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
            v.maxMagicka = MaxMagicka(actor);
            v.onRecord = OnRecord(c, actor);
            const auto taught = TaughtSpells(c);
            for (const KnownSpell &k : KnownSpells(actor, taught))
                v.spells.push_back({k.facts, k.onRecord, Taught(c, k.facts.spell), IsSpellSetAside(c, k.facts.spell)});
            // Set aside, and no longer on their record (a mod updated): still
            // listed, so it can be restored and stop hiding the spell.
            for (const SpellAside &s : c.spellsSetAside)
                if (std::none_of(v.spells.begin(), v.spells.end(),
                                 [&](const KnownSpellRow &r) { return r.facts.spell == s.spell; }))
                {
                    auto *spell = Lookup<RE::SpellItem>(s.spell);
                    SpellFacts facts = spell ? FactsOf(spell, actor) : SpellFacts{};
                    facts.spell = s.spell;
                    if (facts.name.empty())
                        facts.name = s.name;
                    v.spells.push_back({std::move(facts), false, false, true});
                }
            const PerSkill<int> effective = Effective(c, v.base);
            for (const Tome &t : tomes)
            {
                auto *spell = Lookup<RE::SpellItem>(t.facts.spell);
                SpellFacts facts = t.facts;
                if (spell)
                    facts.cost = static_cast<int>(spell->CalculateMagickaCost(actor));
                // One of theirs set aside counts as known: it is restored, not taught.
                const bool known = spell && (actor->HasSpell(spell) || IsSpellSetAside(c, t.facts.spell));
                const TeachStatus status = CanTeach(facts, known, effective, v.maxMagicka);
                v.tomes.push_back({t.book, t.bookName, std::move(facts), t.count, status});
            }
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

// Every action from the panel: on the game thread, under the lock, with
// the views rebuilt after so the panel shows the result on its next frame.
void Act(std::function<void()> work)
{
    OnGameThread([work = std::move(work)] {
        std::scoped_lock lock(g_mutex);
        g_state.refused.clear();
        if (!g_state.inGame)
            return;
        work();
        PublishViews();
        RebuildViews();
        Changed();
    });
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

std::uint64_t Version() noexcept
{
    return g_version.load(std::memory_order_relaxed);
}

Snapshot Read()
{
    std::scoped_lock lock(g_mutex);
    Snapshot s;
    s.companions = g_state.companions;
    s.views = g_state.views;
    s.candidates = g_state.candidates;
    s.settings = g_state.settings;
    s.rules = g_state.rules;
    s.playerLevel = g_state.playerLevel;
    s.inGame = g_state.inGame;
    s.refused = g_state.refused;
    if (g_state.settings.released)
        for (const Companion &c : g_state.companions)
            if (!g_releasedDone.contains(c.key))
                s.stillHeld.push_back(c.name);
    s.version = Version();
    return s;
}

// --- actions ---------------------------------------------------------------------

void Enroll(const FormKey &key)
{
    Act([key = key] {
        RE::Actor *actor = ActorOf(key);
        if (!actor || Find(key))
            return;
        if (!IsUniqueNpc(actor))
        {
            Refuse(fmt::format("{} is not unique: this build trains unique followers only.", NameOf(actor)));
            return;
        }
        Companion &c = EnrollActor(actor, key);
        PublishViews();
        Reconcile(c, actor);
    });
}

void SetPaused(const FormKey &key, bool paused)
{
    Act([key = key, paused] {
        if (Companion *c = Find(key))
        {
            c->paused = paused;
            log::party.info("{}: earning {}", c->name, paused ? "paused" : "resumed");
        }
    });
}

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

void ResetSkill(const FormKey &key, Skill skill)
{
    Act([key = key, skill] {
        auto [c, actor] = Present(key, "reset a skill");
        if (!c)
            return;
        const Rules r = ReadRules();
        const ResetResult reset =
            fp::ResetSkill(*c, skill, BaseSkills(actor)[Index(skill)], FloorOf(actor, skill, r), Graph(), r);
        PublishViews();
        Reconcile(*c, actor);
        log::growth.info("{} reset {}: {:.0f} XP to reassign, {} perk(s) returned", c->name, Name(skill),
                         reset.returned, reset.unlearned.size());
        Hud(fmt::format("{}'s {} is reset: {:.0f} XP to reassign.", c->name, Name(skill), reset.returned));
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
void LearnNode(const FormKey &key, int nodeId, bool quiet)
{
    const auto refuse = [quiet](std::string why) {
        if (quiet)
            RefuseQuietly(std::move(why));
        else
            Refuse(std::move(why));
    };
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
        refuse(fmt::format("{} cannot learn {} now.", c->name, node.name));
        return;
    }
    const int rank = status.held;
    const FormKey &form = node.ranks[static_cast<std::size_t>(rank)].form;
    RE::BGSPerk *perk = PerkOf(form);
    if (!perk)
    {
        refuse(fmt::format("{} is not in this load order.", node.name));
        return;
    }
    fp::Learn(*c, node, rank);
    PublishViews();
    perkview::Reconcile(actor);
    // Through the engine's own HasPerk, which is our ForEachPerk.
    if (!actor->HasPerk(perk))
        log::perks.warn("{}: learned {}, but the engine's HasPerk says no", c->name, node.name);
    log::perks.info("{} learned {} ({}/{}), {}", c->name, node.name, rank + 1, node.ranks.size(), ToString(form));
    if (!quiet)
        Hud(fmt::format("{} learned {}.", c->name, node.name));
}

} // namespace

void LearnPerk(const FormKey &key, int nodeId)
{
    Act([key = key, nodeId] { LearnNode(key, nodeId, false); });
}

void LearnPerkByForm(std::uint32_t actorId, std::uint32_t perkForm)
{
    Act([actorId, perkForm] {
        const auto actorKey = KeyOf(RE::TESForm::LookupByID<RE::Actor>(actorId));
        const auto perkKey = KeyOf(RE::TESForm::LookupByID<RE::BGSPerk>(perkForm));
        if (!actorKey || !perkKey)
            return;
        if (const auto node = Graph().Find(*perkKey))
            LearnNode(*actorKey, node->first, true);
    });
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
        const auto none = [&](const std::string &why) {
            out.buttons.canLower = out.buttons.canRaise = out.buttons.canReset = false;
            out.buttons.lower = out.buttons.raise = out.buttons.reset = why;
        };
        if (g_state.settings.released)
            none("Leveling is off: turn it on in Follower Tactics' Settings.");
        else if (!v.loaded)
            none(c.name + " must be with you for this.");
        out.active = !g_state.settings.released && v.loaded;
        return out;
    }
    return std::nullopt;
}

namespace
{

// The top rank of `nodeId` bought here, given back: on the game thread,
// under the lock.
void UnlearnNode(const FormKey &key, int nodeId, bool quiet)
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
        std::string why = fmt::format("{} cannot unlearn {}: something else needs it.", c->name, node.name);
        if (quiet)
            RefuseQuietly(std::move(why));
        else
            Refuse(std::move(why));
        return;
    }
    const FormKey &form = node.ranks[static_cast<std::size_t>(status.held - 1)].form;
    fp::Unlearn(*c, form);
    PublishViews();
    perkview::Reconcile(actor);
    log::perks.info("{} unlearned {} (rank {})", c->name, node.name, status.held);
    if (!quiet)
        Hud(fmt::format("{} unlearned {}.", c->name, node.name));
}

} // namespace

void UnlearnPerk(const FormKey &key, int nodeId)
{
    Act([key = key, nodeId] { UnlearnNode(key, nodeId, false); });
}

void UnlearnPerkByForm(std::uint32_t actorId, std::uint32_t perkForm)
{
    Act([actorId, perkForm] {
        const auto actorKey = KeyOf(RE::TESForm::LookupByID<RE::Actor>(actorId));
        const auto node = NodeOfPerk(perkForm);
        if (actorKey && node)
            UnlearnNode(*actorKey, *node, true);
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

void CheckViews()
{
    Act([] {
        const perkview::Counters n = perkview::Count();
        const spellview::Counters sn = spellview::Count();
        log::spells.info("hooks so far: {} VisitSpells walks answered from a view ({} of them the combat AI "
                         "gathering), {} casts refused{}",
                         sn.visitsManaged, sn.gathersManaged, sn.castsRefused,
                         spellview::Installed() ? "" : "; the hooks are not installed");
        log::perks.info("hooks so far: {} ForEachPerk walks answered from a view, ApplyPerksFromBase {} ({} for "
                        "companions), {} rank change(s) queued{}",
                        n.forEachPerkManaged, n.applyFromBase, n.applyFromBaseManaged, n.queued,
                        perkview::Installed() ? "" : "; the hooks are not installed");
        std::size_t checked = 0;
        std::size_t clean = 0;
        for (const Companion &c : g_state.companions)
        {
            RE::Actor *actor = ActorOf(c.key);
            if (!IsHere(actor))
                continue;
            const std::string perks = perkview::SelfCheck(actor);
            const std::string spells = spellview::SelfCheck(actor);
            log::perks.info("check: {}", perks);
            log::spells.info("check: {}", spells);
            ++checked;
            const auto fine = [](const std::string &r) {
                return r.find(" expected, engine says ") == std::string::npos &&
                       r.find("CheckCast allows") == std::string::npos;
            };
            clean += fine(perks) && fine(spells) ? 1 : 0;
        }
        Hud(checked == 0 ? std::string("No companion is here to check.")
                         : fmt::format("Perks and spells checked for {} companion(s): {} as expected. Details in "
                                       "the log.",
                                       checked, clean));
    });
}

void Teach(const FormKey &key, const FormKey &spellKey)
{
    Act([key = key, spellKey = spellKey] {
        auto [c, actor] = Present(key, "learn a spell");
        if (!c)
            return;
        const auto tomes = TomesCarried(actor);
        const auto tome =
            std::find_if(tomes.begin(), tomes.end(), [&](const Tome &t) { return t.facts.spell == spellKey; });
        auto *spell = Lookup<RE::SpellItem>(spellKey);
        if (tome == tomes.end() || !spell)
        {
            Refuse("You no longer carry that tome.");
            return;
        }
        if (Taught(*c, spellKey))
        {
            Refuse(fmt::format("{} was taught {} already.", c->name, tome->facts.name));
            return;
        }
        if (IsSpellSetAside(*c, spellKey))
        {
            Refuse(fmt::format("{} knows {} already: it is set aside. Restore it, for nothing.", c->name,
                               tome->facts.name));
            return;
        }
        const TeachStatus status =
            CanTeach(tome->facts, actor->HasSpell(spell), Effective(*c, BaseSkills(actor)), MaxMagicka(actor));
        if (status.block != TeachBlock::None)
        {
            Refuse(fmt::format("{} cannot learn {} yet.", c->name, tome->facts.name));
            return;
        }
        auto *book = Lookup<RE::TESObjectBOOK>(tome->book);
        // Known through the view (progression/game/SpellView.h), not added to the actor;
        // without the hooks, added as the engine keeps it. Checked through
        // the engine's own HasSpell before the tome goes; on failure the
        // ledger is put back exactly as it was.
        const auto spellsBefore = c->spells;
        fp::Teach(*c, tome->facts);
        PublishViews();
        const bool hooked = spellview::Installed();
        const bool known = hooked ? actor->HasSpell(spell) : AddToActor(actor, spell);
        const bool took = known && TakeTome(book);
        if (!took)
        {
            log::spells.warn("{}: {} {}; the tome is kept", c->name, tome->facts.name,
                             known ? "was known, but the tome could not be taken" : "did not take (HasSpell says no)");
            if (known && !hooked)
                actor->RemoveSpell(spell);
            c->spells = spellsBefore;
            Refuse(fmt::format("{} could not learn {}; the tome is kept.", c->name, tome->facts.name));
            return;
        }
        log::spells.info("{} learned {} from {}", c->name, tome->facts.name, tome->bookName);
        Hud(fmt::format("{} learned {}.", c->name, tome->facts.name));
    });
}

void Forget(const FormKey &key, const FormKey &spellKey)
{
    Act([key = key, spellKey = spellKey] {
        auto [c, actor] = Present(key, "forget a spell");
        if (!c || !Taught(*c, spellKey))
            return;
        auto *spell = Lookup<RE::SpellItem>(spellKey);
        const std::string name = NameOf(spell);
        fp::Forget(*c, spellKey);
        PublishViews();
        if (spell)
            ForgetSpell(actor, spell);
        spellview::Withdraw(actor, spell);
        log::spells.info("{} forgot {}", c->name, name);
        Hud(fmt::format("{} forgot {}.", c->name, name));
    });
}

void SetAsideOwnSpell(const FormKey &key, const FormKey &spellKey)
{
    Act([key = key, spellKey = spellKey] {
        auto [c, actor] = Present(key, "set a spell aside");
        if (!c)
            return;
        auto *spell = Lookup<RE::SpellItem>(spellKey);
        if (!spell || !SpellOnRecord(actor, spell) || spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell)
            return;
        if (!spellview::Installed())
        {
            Refuse("Setting a spell aside needs the spell hooks, which are not installed (Skyrim VR).");
            return;
        }
        if (!fp::SetAsideSpell(*c, FactsOf(spell, actor)))
            return;
        PublishViews();
        spellview::Reconcile(actor);
        log::spells.info("{} set {} aside", c->name, NameOf(spell));
        Hud(fmt::format("{} set {} aside.", c->name, NameOf(spell)));
    });
}

void RestoreOwnSpell(const FormKey &key, const FormKey &spellKey)
{
    Act([key = key, spellKey = spellKey] {
        auto [c, actor] = Present(key, "take a spell up again");
        if (!c || !fp::RestoreSpell(*c, spellKey))
            return;
        const std::string name = NameOf(Lookup<RE::SpellItem>(spellKey));
        log::spells.info("{} took {} up again", c->name, name);
        Hud(fmt::format("{} took {} up again.", c->name, name));
    });
}

void ChangeSettings(const Settings &settings)
{
    Act([settings] {
        // Released is the actions' below, not the checkboxes'.
        const bool released = g_state.settings.released;
        g_state.settings = settings;
        g_state.settings.released = released;
    });
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
        const PerSkill<int> base = BaseSkills(actor);
        const int floor = FloorOf(actor, skill, r);
        const int step = direction < 0 ? -1 : +1;
        int moved = 0;
        // As far as - or + would each go; bounded, since each step moves a
        // level and the skill has at most the cap's worth.
        for (int guard = 0; guard <= r.skillCap; ++guard)
        {
            const Holdings holdings = HoldingsOf(*c, OnRecord(*c, actor));
            if (CheckSkill(*c, skill, step, base, floor, Graph(), holdings, r).block != AssignBlock::None)
                break;
            fp::AssignSkill(*c, skill, step, base[Index(skill)], r);
            ++moved;
        }
        if (moved == 0)
            return;
        Reconcile(*c, actor);
        log::growth.info("{}: {} {} by {}", c->name, Name(skill), step < 0 ? "lowered" : "raised", moved);
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

void Gift(const FormKey &key, double xp)
{
    Act([key = key, xp] {
        for (Companion &c : g_state.companions)
        {
            if (!key.Empty() && c.key != key)
                continue;
            if (key.Empty() && !g_following.contains(c.key))
                continue;
            RE::Actor *actor = ActorOf(c.key);
            fp::Gift(c, xp);
            if (IsHere(actor))
            {
                NoteLevel(c, actor, ReadRules());
                Reconcile(c, actor);
            }
        }
    });
}

void DumpPerks()
{
    Act([] {
        const auto path = DumpGraph();
        if (path.empty())
            Refuse("The perk graph could not be written.");
        else
        {
            log::perks.info("perk graph written to {}", path.string());
            Hud("Perk graph written to the SKSE log folder.");
        }
    });
}

void PanelShown(bool shown)
{
    g_panelShown.store(shown, std::memory_order_relaxed);
    g_drawn.store(shown, std::memory_order_relaxed);
    if (shown)
        OnGameThread([] {
            std::scoped_lock lock(g_mutex);
            if (!g_state.inGame)
                return;
            RebuildViews();
            Changed();
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

    // Who is here: enrol the new, note who is following, list the rest.
    const std::vector<RE::Actor *> followers = LoadedFollowers();
    std::vector<Candidate> candidates;
    std::unordered_set<FormKey, FormKeyHash> following;
    for (RE::Actor *actor : followers)
    {
        const auto key = KeyOf(actor);
        if (!key)
            continue;
        if (!Find(*key))
        {
            if (g_state.settings.autoEnroll && !g_state.settings.released && IsUniqueNpc(actor))
            {
                EnrollActor(actor, *key);
                PublishViews();
            }
            else
            {
                candidates.push_back({*key, NameOf(actor), IsUniqueNpc(actor)});
                continue;
            }
        }
        if (!IsWaiting(actor))
            following.insert(*key);
    }
    g_following = std::move(following);
    g_state.candidates = std::move(candidates);

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

    if (g_panelShown.load(std::memory_order_relaxed) && g_drawn.exchange(false, std::memory_order_relaxed))
        RebuildViews();
    Changed();
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
    Changed();
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
    Changed();
}

void OnGameStarted()
{
    auto *player = RE::PlayerCharacter::GetSingleton();
    std::scoped_lock lock(g_mutex);
    g_state.inGame = true;
    g_state.playerLevel = player ? player->GetLevel() : 1;
    g_following.clear();
    PublishViews();
    Changed();
}

void OnGameLeft()
{
    std::scoped_lock lock(g_mutex);
    g_state.inGame = false;
    Changed();
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

void NoteDrawn() noexcept
{
    g_drawn.store(true, std::memory_order_relaxed);
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
    Changed();
}

void Revert()
{
    std::scoped_lock lock(g_mutex);
    perkview::Forget();
    spellview::Forget();
    learning::Forget();
    g_published.clear();
    g_following.clear();
    g_releasedDone.clear();
    g_state = State{};
    Changed();
}

} // namespace fp::game
