#include "game/AiScore.h"

#include "core/AttackScore.h"
#include "core/Variety.h"
#include "game/Bag.h"
#include "game/Inventory.h"
#include "game/Log.h"
#include "game/Packages.h"
#include "game/Pins.h"
#include "game/Sensors.h"
#include "game/Settings.h"
#include "game/Util.h"
#include "game/Values.h"

#include <algorithm>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ft::game
{
namespace
{

// The engine's attack category: melee, bows and attack spells, the one
// category whose entries compete with each other for a hand on score
// alone (dev/COMBAT_AI.md 3). Heals, wards and buffs are chosen by their
// gates, and are left to the engine.
constexpr int kOffence = 0;

std::uint64_t FreshSeed();

// One follower's variety, for one fight: a new combat controller is a new
// fight, and starts it over. Entries are named by their address, which the
// engine keeps for the fight.
struct Choice
{
    const RE::CombatController *controller{nullptr};
    ft::Variety variety{FreshSeed()};
    // What the log has said this fight, once each: an entry's figure per
    // draw, a stand-down or an immunity per entry.
    std::unordered_set<const void *> logged;
    std::unordered_set<const void *> heldBack;
    // Entries the engine files elsewhere or scores 0 or less, said once a
    // fight: an attack spell the AI never uses is found here.
    std::unordered_set<const void *> listed;
    // A spell cast on oneself, scored here: how many enemies its rings last
    // reached, so the log says each time that changes.
    std::unordered_map<const void *, int> selfArea;
    // Entries the engine's equip check refused and ours allowed, said once
    // a fight.
    std::unordered_set<const void *> allowed;
    // The loadout last logged, so it is said once each time it changes.
    std::string loadout;
    // Each spell's magicka cost, priced once a fight: the engine's cost
    // walks the perks, and was the slow step of the player's snapshot.
    std::unordered_map<std::uint32_t, float> costs;
    // Every attack entry's latest answer, by name, for the line each cast
    // writes: what the choice was made against. Kept at debug only.
    struct Last
    {
        std::string name;
        float score{0.0f};
    };
    std::unordered_map<const void *, Last> last;
};

// The AI scores on its own threads, the cast sink hears the game's, the
// tick writes the waits: one lock over all of it.
std::mutex g_mutex;
std::unordered_map<std::uint32_t, Choice> g_choices;
std::unordered_set<std::uint32_t> g_waiting;

std::uint64_t FreshSeed()
{
    static std::random_device device;
    return (static_cast<std::uint64_t>(device()) << 32) ^ device();
}

// The engine's categories, in the order its loadout offers them a hand:
// 1, 2, 4, 0, 3, 5, 0, 6 (dev/COMBAT_AI.md 3). Which of 2 and 3 holds the
// long ward is not settled.
const char *CategoryName(int category)
{
    switch (category)
    {
    case 0:
        return "attack";
    case 1:
        return "restore";
    case 2:
    case 3:
        return "ward or defence";
    case 4:
        return "long buff";
    case 5:
        return "short buff";
    case 6:
        return "block";
    default:
        return "?";
    }
}

const char *HandTagOf(const RE::CombatInventoryItem *entry)
{
    switch (entry->itemSlot.equipSlot ? entry->itemSlot.equipSlot->GetFormID() : 0)
    {
    case kLeftHandSlot:
        return "[L]";
    case kRightHandSlot:
        return "[R]";
    default:
        return "";
    }
}

// A spell, a scroll or a staff: what a rule's cast and the AI's own contend
// for. Not a potion: drinking is no cast, and a potion held back left
// Serana a fight without one (2026-09-23).
bool IsCasting(const RE::TESForm *item)
{
    if (item->As<RE::SpellItem>())
        return true;
    const auto *weapon = item->As<RE::TESObjectWEAP>();
    return weapon && weapon->IsStaff();
}

// Their perks and damage effects, as a factor on the engine's figure: what
// WeaponDamage makes of the weapon against the enemy, over the plain
// figure the engine scores (base times the skill curve, plus melee
// damage; 26410, dev/COMBAT_AI.md 2). Neither counts tempering or the
// weapon's enchantment: the engine's entry names the form, not the copy.
float WeaponFactor(RE::Actor *actor, RE::TESObjectWEAP *weapon, RE::Actor *target)
{
    const float base = weapon->GetAttackDamage();
    if (!(base > 0.0f))
        return 1.0f;
    auto *owner = actor->AsActorValueOwner();
    const float flat = owner ? owner->GetActorValue(RE::ActorValue::kMeleeDamage) : 0.0f;
    const float plain = base * SkillCurveOf(actor, weapon).factor + flat;
    const float ours = WeaponDamage(actor, weapon, nullptr, nullptr, target);
    return plain > 0.0f && ours > 0.0f ? ours / plain : 1.0f;
}

// The magnitude perks against the enemy (Augmented Flames, a perk against
// undead), as a factor on the record's magnitude, which is what the engine
// scores: the costliest effect's, the spell's own measure of what it is.
float MagnitudeFactor(RE::Actor *actor, RE::MagicItem *magic, RE::Actor *target)
{
    const RE::Effect *costliest = magic->GetCostliestEffectItem();
    if (!costliest || !(costliest->effectItem.magnitude > 0.0f))
        return 1.0f;
    const float actual = ActualMagnitude(actor, magic, costliest, target);
    return actual > 0.0f ? actual / costliest->effectItem.magnitude : 1.0f;
}

// Does every hostile effect's conditions spare this enemy? Asked as the
// engine asks when the effect lands: the enemy as Subject, the caster as
// Target, both lists, the spell's entry's and the effect record's
// (EffectRows.cpp, EffectEntryRow). The engine's score counts resistances and
// nothing of these, and its attack-spell gate does not ask them either
// (dev/COMBAT_AI.md 4): a paralysis on an automaton, a drain on the undead.
bool SparedByConditions(RE::MagicItem *magic, RE::Actor *caster, RE::Actor *target)
{
    bool anyHostile = false;
    for (const auto *effect : ResolvedEffects(*magic))
    {
        const auto *base = effect->baseEffect;
        if (!base->IsHostile())
            continue;
        anyHostile = true;
        const bool entryHolds = !effect->conditions.head || effect->conditions.IsTrue(target, caster);
        const bool recordHolds = !base->conditions.head || base->conditions.IsTrue(target, caster);
        if (entryHolds && recordHolds)
            return false;
    }
    return anyHostile;
}

// Their spells stand down: a rule of theirs is waiting on the cast in their
// hands, or one of our records is casting for them. The spell our record
// casts is not held back; the package's own equip of it goes through the
// AI's list.
bool HoldsBack(RE::Actor *actor, const RE::TESForm *item)
{
    if (!IsCasting(item) || IsOurCast(actor, item->GetFormID()))
        return false;
    if (IsMidCast(actor))
        return true;
    std::scoped_lock lock(g_mutex);
    return g_waiting.contains(actor->GetFormID());
}

// The entry's reach as the loadout reads it (slots 05-08): the range an
// entry is scored down by x0.1 outside (dev/COMBAT_AI.md 3).
std::string ReachOf(RE::CombatInventoryItem *entry)
{
    const auto show = [](float range) { return range >= 1.0e30f ? std::string("any") : fmt::format("{:.0f}", range); };
    return fmt::format("reach {} to {}, optimal {}, equip within {}", show(entry->GetMinRange()),
                       show(entry->GetMaxRange()), show(entry->GetOptimalRange()), show(entry->GetEquipRange()));
}

// An equipment set as the loadout builds it (dev/COMBAT_AI.md 3): items at
// 0x00, the slot mask at 0x18 and the set's score at 0x1C, where
// CommonLib's header has maxRange -- its fields after the mask are
// misnamed, so the score is read by offset.
std::string SetOf(const RE::CombatEquipment &set)
{
    std::string items;
    for (const auto &item : set.items)
        if (item)
            items += (items.empty() ? "" : ", ") +
                     fmt::format("{}{} {:.2f}", log::NameOf(item->item), HandTagOf(item.get()), item->itemScore);
    const float score = *reinterpret_cast<const float *>(reinterpret_cast<const std::byte *>(&set) + 0x1C);
    return fmt::format("[{}] score {:.2f}", items.empty() ? std::string("nothing") : items, score);
}

// A staff's charge as the Inventory tab reads it -- the copy's, or the
// hand's live value while it is held -- against its full charge and one
// use's cost: a staff the AI passes over may be one it counts as empty.
// Empty for anything but a staff.
std::string StaffChargeOf(RE::Actor *actor, RE::TESForm *item)
{
    auto *weapon = item->As<RE::TESObjectWEAP>();
    if (!weapon || !weapon->IsStaff())
        return "";
    const WeaponCharge charge = ChargeOf(actor, weapon, Hand::None);
    if (!charge.enchanted)
        return "; no enchantment to charge";
    return fmt::format("; staff charge {:.0f} / {:.0f}, {:.0f} a use", charge.charge, charge.maxCharge,
                       charge.costPerHit);
}

// Once per entry per something: the log's own bookkeeping, under the lock.
bool FirstTime(std::unordered_set<const void *> &seen, const void *entry)
{
    return seen.insert(entry).second;
}

// Their choice, for this fight: a controller not seen before is a new
// fight, and everything of the last is forgotten. Under the lock.
Choice &ChoiceFor(RE::Actor *actor, const RE::CombatController *controller, double now)
{
    Choice &choice = g_choices[actor->GetFormID()];
    if (choice.controller != controller)
    {
        choice.controller = controller;
        choice.variety.Reset(now);
        choice.logged.clear();
        choice.heldBack.clear();
        choice.listed.clear();
        choice.selfArea.clear();
        choice.allowed.clear();
        choice.loadout.clear();
        choice.costs.clear();
        choice.last.clear();
    }
    return choice;
}

// What the engine charges an entry to hold it -- magicka for a spell,
// charge for a staff -- as its loadout reads it (slot 0x10): an entry
// scored first and never equipped may be one it cannot pay for.
// With what they have of it now: a staff's charge is the hand's
// (RightItemCharge, LeftItemCharge), the charge of whatever that hand
// holds, which is the suspect in a staff never drawn (dev/COMBAT_AI.md 0).
std::string EngineChargeOf(RE::CombatInventoryItem *entry, RE::Actor *actor)
{
    RE::CombatInventoryItemResource resource{};
    if (!entry->GetResource(resource))
        return "the engine charges nothing";
    auto *owner = actor->AsActorValueOwner();
    const auto av = resource.actorValue;
    const std::string name = av == RE::ActorValue::kMagicka           ? std::string("Magicka")
                             : av == RE::ActorValue::kRightItemCharge ? std::string("RightItemCharge")
                             : av == RE::ActorValue::kLeftItemCharge
                                 ? std::string("LeftItemCharge")
                                 : fmt::format("actor value {}", static_cast<int>(av));
    return fmt::format("the engine charges {:.1f} of {}, of which they have {:.1f}", resource.value, name,
                       owner ? owner->GetActorValue(av) : 0.0f);
}

// Their pool as the price reads it: magicka now and at most, and what comes
// back a second in a fight, as the engine's regeneration (38460) has it --
// the rate (percent of the maximum a second), its multiplier (percent; at
// 0 nothing comes back), and the game's own slowing of it in combat.
ft::MagickaPool PoolOf(RE::Actor *actor)
{
    auto *owner = actor->AsActorValueOwner();
    if (!owner)
        return {};
    static const float combatMult = [] {
        const float mult = GameSetting("fCombatMagickaRegenRateMult", 0.33f);
        log::ai.debug("magicka in a fight comes back at x{:.2f} (fCombatMagickaRegenRateMult)", mult);
        return mult;
    }();
    ft::MagickaPool pool;
    pool.current = owner->GetActorValue(RE::ActorValue::kMagicka);
    pool.max = owner->GetPermanentActorValue(RE::ActorValue::kMagicka);
    const float rate = owner->GetActorValue(RE::ActorValue::kMagickaRate);
    const float rateMult = owner->GetActorValue(RE::ActorValue::kMagickaRateMult);
    pool.regenPerSecond = (std::max)(0.0f, pool.max * rate / 100.0f * rateMult / 100.0f * combatMult);
    return pool;
}

// The AI's hold before release, by their combat style (45354); the
// style's middle where there is none.
double HoldOf(const RE::CombatController *controller)
{
    const auto *style = controller->combatStyle;
    return ft::HoldSeconds(style ? style->generalData.offensiveMult : 0.5);
}

// The loadout the engine built last -- the set it equips from, the set it
// would want at any range, and what is in hand -- logged when it changes.
// Read at the first entry of a rescore, on the AI's own thread, where the
// sets are as the last pass left them and the next pass has not begun.
// Debug only.
void LogLoadoutIfNew(RE::CombatInventoryItem *entry, RE::CombatController *controller, RE::Actor *actor)
{
    auto *inventory = controller->inventory;
    if (!inventory)
        return;
    const RE::CombatInventoryItem *first = nullptr;
    for (const auto &items : inventory->inventoryItems)
        if (!items.empty())
        {
            first = items[0].get();
            break;
        }
    if (entry != first)
        return;
    std::string held;
    for (const auto &equipped : inventory->equippedItems)
        if (equipped.item)
            held += (held.empty() ? "" : ", ") +
                    fmt::format("{}{}", log::NameOf(equipped.item->item), HandTagOf(equipped.item.get()));
    const std::string equips = SetOf(inventory->unk148);
    const std::string wants = SetOf(inventory->unk118);
    // The range fields the queue's range test (44904) reads, raw and by
    // offset: CommonLib's names for them are not trusted (dev/COMMONLIB.md).
    const auto *raw = reinterpret_cast<const float *>(reinterpret_cast<const std::byte *>(inventory) + 0x1A8);
    const std::string text = fmt::format(
        "equips {}; at any range {}; in hand [{}]; ranges 1A8 {:.0f}, 1AC {:.0f}, 1B0 {:.0f}, 1B4 {:.0f}, 1B8 {:.0f}, "
        "1BC {:.0f}, 1C0 {:.0f}",
        equips, wants, held.empty() ? std::string("nothing") : held, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
        raw[6]);
    {
        std::scoped_lock lock(g_mutex);
        Choice &choice = ChoiceFor(actor, controller, TacticsSeconds());
        if (choice.loadout == equips + wants + held)
            return;
        choice.loadout = equips + wants + held;
    }
    log::ai.debug("{} AI loadout: {}", Describe(actor), text);
}

class CastSink : public RE::BSTEventSink<RE::TESSpellCastEvent>
{
  public:
    RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent *ev,
                                          RE::BSTEventSource<RE::TESSpellCastEvent> *) override
    {
        if (!ev || !ev->object || ev->spell == 0)
            return RE::BSEventNotifyControl::kContinue;
        const std::uint32_t who = ev->object->GetFormID();
        std::vector<Choice::Last> scores;
        {
            std::scoped_lock lock(g_mutex);
            // Only a follower mid-fight has a choice to vary.
            const auto it = g_choices.find(who);
            if (it == g_choices.end())
                return RE::BSEventNotifyControl::kContinue;
            it->second.variety.NoteCast(ev->spell);
            for (const auto &[entry, scored] : it->second.last)
                scores.push_back(scored);
        }
        if (scores.empty() || !log::Enabled(log::Level::Debug))
            return RE::BSEventNotifyControl::kContinue;
        std::ranges::sort(scores, [](const Choice::Last &a, const Choice::Last &b) { return a.score > b.score; });
        std::string list;
        for (const auto &scored : scores)
            list += (list.empty() ? "" : ", ") + fmt::format("{} {:.2f}", scored.name, scored.score);
        auto *actor = ev->object->As<RE::Actor>();
        const ft::MagickaPool pool = actor ? PoolOf(actor) : ft::MagickaPool{};
        log::ai.debug("{} cast {} -- magicka {:.0f}/{:.0f}; attack scores: {}", Describe(actor),
                      log::NameOf(RE::TESForm::LookupByID(ev->spell)), pool.current, pool.max, list);
        return RE::BSEventNotifyControl::kContinue;
    }
};

CastSink g_castSink;

// What an effect's area, in feet as the record gives it, reaches in game
// units. 64/3 is the engine's foot where it converts one: a cloak's
// magnitude, in feet, is its radius at that many units (34243, the only
// reader of the constant on 1.6.1170). That an area takes the same foot is
// INFERRED; the log says each distance and radius, to be read against a cast.
constexpr float kUnitsPerFoot = 64.0f / 3.0f;

// A hostile spell cast on oneself -- Fire Storm and its kind -- the engine
// scores nothing: its registry has no hostile entry for a self-delivered
// spell (dev/MAGIC.md, "Which spells the combat AI can use at all"). Scored
// here as the engine scores an aimed one (dev/COMBAT_AI.md 2): the style's
// magic multiplier times each damage effect's magnitude x its duration, at
// least a second, times the engine's weight for what it damages (health 1,
// magicka 0.5, stamina 0.33), less the enemy's resistance to it -- for
// every enemy the effect's area reaches, each at its own distance, so the
// rings of Cold Fire Storm add up to what would land on the crowd around
// the caster. Its stagger, slow and the rest
// are not damage and count for nothing, as the engine's damage entry has
// them. None reach, and it is 0: cast now it would hit nobody. A player who
// does not want these cast bans them.
//
// The engine gives such a spell no reach either -- 0 to 0, so its loadout
// counts it at a tenth however close the enemy (dev/COMBAT_AI.md 3) -- and
// `reach` is the largest ring's radius, which the entry is given.
struct SelfArea
{
    float score{0.0f};
    float distance{0.0f};
    float reach{0.0f};
    int enemies{0};    // the enemies it was asked against
    int reached{0};    // the most any one ring reaches
    std::string rings; // "60 ft 60 x3, 40 ft 20 x1", each ring and how many it reaches, for the log
};

// The engine's weight for damage to a value (dev/COMBAT_AI.md 2), 0 for an
// effect that is not damage.
float DamageWeight(const RE::Effect &effect)
{
    const auto *base = effect.baseEffect;
    using Archetype = RE::EffectSetting::Archetype;
    if (!base->IsHostile() || effect.effectItem.area == 0 ||
        !(base->HasArchetype(Archetype::kValueModifier) || base->HasArchetype(Archetype::kPeakValueModifier) ||
          base->HasArchetype(Archetype::kDualValueModifier)))
        return 0.0f;
    switch (base->data.primaryAV)
    {
    case RE::ActorValue::kHealth:
        return 1.0f;
    case RE::ActorValue::kMagicka:
        return 0.5f;
    case RE::ActorValue::kStamina:
        return 0.33f;
    default:
        return 0.0f;
    }
}

// Whom the caster's side is fighting: the combat group's targets, read
// under the group's lock and resolved outside it, the dead left out; the
// controller's own target among them if the group has not listed it yet.
std::vector<RE::NiPointer<RE::Actor>> EnemiesOf(const RE::CombatController *controller, RE::Actor *target)
{
    std::vector<RE::ActorHandle> handles;
    if (auto *group = controller ? controller->combatGroup : nullptr)
    {
        const RE::BSReadLockGuard locker(group->lock);
        for (const auto &listed : group->targets)
            handles.push_back(listed.targetHandle);
    }
    std::vector<RE::NiPointer<RE::Actor>> enemies;
    for (const auto &handle : handles)
        if (auto enemy = handle.get(); enemy && !enemy->IsDead())
            enemies.push_back(std::move(enemy));
    if (target && !target->IsDead() &&
        std::ranges::none_of(enemies, [&](const auto &enemy) { return enemy.get() == target; }))
        enemies.emplace_back(target);
    return enemies;
}

SelfArea SelfAreaScore(RE::Actor *caster, RE::MagicItem *spell, RE::Actor *target,
                       const RE::CombatController *controller)
{
    SelfArea out;
    if (!caster || !spell || spell->GetDelivery() != RE::MagicSystem::Delivery::kSelf)
        return out;
    // Every enemy, each at its own distance and with its own resistance:
    // what the rings would land on, summed. With none the rings still give
    // the reach, and reach nobody.
    const auto enemies = EnemiesOf(controller, target);
    std::vector<float> distances;
    distances.reserve(enemies.size());
    for (const auto &enemy : enemies)
        distances.push_back(caster->GetPosition().GetDistance(enemy->GetPosition()));
    out.distance = target ? caster->GetPosition().GetDistance(target->GetPosition()) : FLT_MAX;
    static const float maxResist = GameSetting("fPlayerMaxResistance", 85.0f);
    double sum = 0.0;
    for (const RE::Effect *effect : ResolvedEffects(*spell))
    {
        const auto *base = effect->baseEffect;
        const float weight = DamageWeight(*effect);
        if (!(weight > 0.0f))
            continue;
        const float radius = static_cast<float>(effect->effectItem.area) * kUnitsPerFoot;
        out.reach = (std::max)(out.reach, radius);
        int reached = 0;
        for (std::size_t i = 0; i < enemies.size(); ++i)
        {
            if (distances[i] > radius)
                continue;
            ++reached;
            double magnitude = effect->effectItem.magnitude;
            auto *owner = enemies[i]->AsActorValueOwner();
            if (const auto resist = base->data.resistVariable; owner && resist != RE::ActorValue::kNone)
                magnitude *= 1.0 - std::clamp(owner->GetActorValue(resist), -100.0f, maxResist) / 100.0;
            sum += magnitude * weight * (std::max)(1u, effect->effectItem.duration);
        }
        out.reached = (std::max)(out.reached, reached);
        out.rings += fmt::format("{}{} ft {:.0f} x{}", out.rings.empty() ? "" : ", ", effect->effectItem.area,
                                 effect->effectItem.magnitude, reached);
    }
    out.enemies = static_cast<int>(enemies.size());
    const auto *style = controller ? controller->combatStyle : nullptr;
    out.score = static_cast<float>(sum * (style ? style->generalData.magicScoreMult : 1.0f));
    return out;
}

} // namespace

float FollowerScore(RE::CombatInventoryItem *entry, RE::CombatController *controller, RE::Actor *actor, float engine)
{
    if (!entry || !entry->item || !controller || !actor || actor->IsPlayerRef() || !actor->IsPlayerTeammate())
        return engine;
    RE::TESForm *item = entry->item;

    if (HoldsBack(actor, item))
    {
        bool first = false;
        {
            // An enemy it cannot touch: its draw goes with its turn.
            std::scoped_lock lock(g_mutex);
            Choice &choice = ChoiceFor(actor, controller, TacticsSeconds());
            choice.variety.Release(reinterpret_cast<std::uintptr_t>(entry));
            first = FirstTime(choice.heldBack, entry);
        }
        if (first)
            log::ai.debug("{} AI's {}{} stands down: a rule of theirs is casting, or waiting to", Describe(actor),
                          log::NameOf(item), HandTagOf(entry));
        return 0.0f;
    }

    const int category = static_cast<int>(entry->GetCategory());
    if (log::Enabled(log::Level::Debug))
    {
        LogLoadoutIfNew(entry, controller, actor);
        bool first = false;
        {
            std::scoped_lock lock(g_mutex);
            first = FirstTime(ChoiceFor(actor, controller, TacticsSeconds()).listed, entry);
        }
        if (first)
            log::ai.debug("{} AI entry {}{}: category {} ({}), filed with the {}, engine score {:.2f}{}; {}{}",
                          Describe(actor), log::NameOf(item), HandTagOf(entry), category, CategoryName(category),
                          CombatEntryClass(entry), engine, engine > 0.0f ? "" : " -- never queued", ReachOf(entry),
                          StaffChargeOf(actor, item));
    }
    // The rest is the Settings page's two switches, "Use self-targeting
    // damage spells" and "Varied AI choices"; the stand-down above is
    // tactics' own, and stays whatever they say.
    const ft::Settings settings = CurrentSettings();
    if (category != kOffence || (!settings.variedAiChoices && !settings.selfDamageSpells))
        return engine;
    const RE::NiPointer<RE::Actor> target = controller->targetHandle.get();
    // A hostile spell cast on oneself, with its switch on (SelfAreaScore).
    // Its reach goes on the entry, so the loadout does not count it at a
    // tenth: the largest ring's radius, where the engine reads it (slots 06
    // and 07 return maxRange and a point between it and minRange,
    // CommonLib's names for +0x34 and +0x30, read on 1.5.97 and 1.6.1170).
    // The entry is a spell's or a scroll's, a CombatInventoryItemMagic.
    auto *spell = item->As<RE::TESObjectWEAP>() ? nullptr : item->As<RE::MagicItem>();
    const SelfArea area =
        settings.selfDamageSpells && spell ? SelfAreaScore(actor, spell, target.get(), controller) : SelfArea{};
    if (area.reach > 0.0f)
        static_cast<RE::CombatInventoryItemMagic *>(entry)->maxRange = area.reach;
    // The engine's own 0. Such a spell is scored here instead; anything
    // else cannot be used now, and gives up its draw (core/Variety.h).
    if (!(engine > 0.0f))
    {
        if (!area.rings.empty() && log::Enabled(log::Level::Debug))
        {
            bool changed = false;
            {
                std::scoped_lock lock(g_mutex);
                auto &seen = ChoiceFor(actor, controller, TacticsSeconds()).selfArea;
                const auto [it, fresh] = seen.try_emplace(entry, area.reached);
                changed = fresh || it->second != area.reached;
                it->second = area.reached;
            }
            if (changed)
                log::ai.debug("{} AI score of {}{}: cast on themself, the engine's {:.2f} replaced -- {} enemies, {} "
                              "at {:.0f} units; rings reach {} -> {:.2f}",
                              Describe(actor), log::NameOf(item), HandTagOf(entry), engine, area.enemies,
                              target ? Describe(target.get()) : std::string("nobody"), area.distance, area.rings,
                              area.score);
        }
        if (!(area.score > 0.0f))
        {
            std::scoped_lock lock(g_mutex);
            ChoiceFor(actor, controller, TacticsSeconds()).variety.Release(reinterpret_cast<std::uintptr_t>(entry));
            return engine;
        }
        engine = area.score;
    }
    // Scored as the engine scores; the rest is variety's.
    if (!settings.variedAiChoices)
        return engine;

    auto *weapon = item->As<RE::TESObjectWEAP>();
    if (weapon && !weapon->IsStaff())
    {
        const float factor = WeaponFactor(actor, weapon, target.get());
        const float score = engine * factor;
        bool first = false;
        {
            std::scoped_lock lock(g_mutex);
            first = FirstTime(ChoiceFor(actor, controller, TacticsSeconds()).logged, entry);
        }
        if (first)
            log::ai.debug("{} AI score of {}{}: engine {:.2f} a second, perks and effects x{:.2f} against {} -> {:.2f}",
                          Describe(actor), log::NameOf(item), HandTagOf(entry), engine, factor,
                          target ? Describe(target.get()) : std::string("nobody"), score);
        return score;
    }

    // A staff is a weapon record whose cast is its enchantment: scored as
    // that spell would be, the perks and conditions asked of it.
    RE::MagicItem *magic = weapon ? weapon->formEnchanting : item->As<RE::MagicItem>();
    if (!magic)
        return engine;
    if (target && SparedByConditions(magic, actor, target.get()))
    {
        bool first = false;
        {
            std::scoped_lock lock(g_mutex);
            first = FirstTime(ChoiceFor(actor, controller, TacticsSeconds()).heldBack, entry);
        }
        if (first)
            log::ai.debug("{} AI score of {}{}: engine {:.2f}, answered 0 -- every hostile effect spares {}",
                          Describe(actor), log::NameOf(item), HandTagOf(entry), engine, Describe(target.get()));
        return 0.0f;
    }
    const float factor = MagnitudeFactor(actor, magic, target.get());
    // The engine's figure is what one cast does, with its resistances,
    // its kind's weight and the style's multiplier in it; ours is that per
    // second of their time, magicka counted as time (core/AttackScore.h).
    const float perCycle = engine * factor;
    const bool stream = magic->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
    // A scroll is spent, not paid for, and kept back as the backup for when
    // magicka runs low; a staff spends its charge, which the recharge rule
    // answers, not the score.
    const bool scroll = item->As<RE::ScrollItem>() != nullptr;
    const bool costless = weapon || scroll;
    const double now = TacticsSeconds();
    float cost = 0.0f;
    if (!costless)
    {
        bool priced = false;
        {
            std::scoped_lock lock(g_mutex);
            Choice &choice = ChoiceFor(actor, controller, now);
            if (const auto it = choice.costs.find(magic->GetFormID()); it != choice.costs.end())
            {
                cost = it->second;
                priced = true;
            }
        }
        if (!priced)
        {
            // A stream's cost is by the second: the caster's update (34143)
            // drains the spell's cost times each frame's time while it
            // streams.
            cost = magic->CalculateMagickaCost(actor);
            std::scoped_lock lock(g_mutex);
            ChoiceFor(actor, controller, now).costs[magic->GetFormID()] = cost;
        }
    }
    static const float streamSeconds = GameSetting("fCombatMagicConcentrationScoreDuration", 3.0f);
    const double charge = magic->GetChargeTime();
    const double hold = HoldOf(controller);
    const ft::SpellCycle cycle = stream ? ft::StreamCycle(streamSeconds, cost) : ft::ReleasedCycle(charge, hold, cost);
    const ft::MagickaPool pool = weapon ? ft::MagickaPool{} : PoolOf(actor);
    const double price = costless ? 0.0 : ft::MagickaPrice(pool);
    const double reserve = scroll ? ft::ScrollReserve(pool) : 1.0;
    const auto perSecond = static_cast<float>(ft::PerSecond(perCycle, cycle, price) * reserve);

    ft::Variety::Varied varied;
    {
        std::scoped_lock lock(g_mutex);
        Choice &choice = ChoiceFor(actor, controller, now);
        varied = choice.variety.Adjust(reinterpret_cast<std::uintptr_t>(entry), magic->GetFormID(), perSecond, now,
                                       target ? target->GetFormID() : 0);
    }
    // Once per draw: a draw is held until its spell has had its turn.
    if (varied.fresh && log::Enabled(log::Level::Debug))
    {
        const std::string timing = stream ? fmt::format("a stream over {:.1f} s", cycle.seconds)
                                          : fmt::format("charge {:.2f} s + hold {:.2f} s", charge, hold);
        const std::string paid =
            scroll ? fmt::format("a scroll, kept back to x{:.2f} (pool {:.0f}/{:.0f})", reserve, pool.current, pool.max)
            : costless
                ? std::string("no magicka")
                : fmt::format("{:.0f} magicka{} at {:.3f} s a point (pool {:.0f}/{:.0f}, back {:.1f}/s)", cycle.magicka,
                              stream ? " over it" : "", price, pool.current, pool.max, pool.regenPerSecond);
        log::ai.debug("{} AI score of {}{}: engine {:.2f} a cast, perks x{:.2f} -> {:.2f} a cast; {}; {} -> {:.2f} a "
                      "second; recent x{:.2f}, draw x{:.2f} -> {:.2f} (draw {}); {}{}",
                      Describe(actor), log::NameOf(item), HandTagOf(entry), engine, factor, perCycle, timing, paid,
                      perSecond, varied.recency, varied.draw, varied.score, varied.drawn, EngineChargeOf(entry, actor),
                      StaffChargeOf(actor, item));
    }
    return varied.score;
}

bool SelfDamageMayEquip(RE::CombatInventoryItem *entry, RE::CombatController *controller, RE::Actor *actor)
{
    if (!entry || !entry->item || !controller || !actor || actor->IsPlayerRef() || !actor->IsPlayerTeammate() ||
        !CurrentSettings().selfDamageSpells || static_cast<int>(entry->GetCategory()) != kOffence ||
        !(entry->itemScore > 0.0f) || (controller->state && controller->state->isFleeing))
        return false;
    auto *spell = entry->item->As<RE::TESObjectWEAP>() ? nullptr : entry->item->As<RE::MagicItem>();
    if (!spell || spell->GetDelivery() != RE::MagicSystem::Delivery::kSelf ||
        std::ranges::none_of(ResolvedEffects(*spell),
                             [](const RE::Effect *effect) { return DamageWeight(*effect) > 0.0f; }))
        return false;
    // What the engine charges to hold it, against what they have: the
    // afford check its own gates make first.
    if (RE::CombatInventoryItemResource resource{}; entry->GetResource(resource))
    {
        auto *owner = actor->AsActorValueOwner();
        if (!owner || owner->GetActorValue(resource.actorValue) < resource.value)
            return false;
    }
    bool first = false;
    {
        std::scoped_lock lock(g_mutex);
        first = ChoiceFor(actor, controller, TacticsSeconds()).allowed.insert(entry).second;
    }
    if (first)
        log::ai.debug("{} AI's equip check refused {}{} (filed with the {}); allowed as a self-targeting damage spell, "
                      "scored {:.2f}",
                      Describe(actor), log::NameOf(entry->item), HandTagOf(entry), CombatEntryClass(entry),
                      entry->itemScore);
    return true;
}

void NoteAnswer(RE::CombatInventoryItem *entry, RE::CombatController *controller, RE::Actor *actor, float answer)
{
    if (!log::Enabled(log::Level::Debug) || !entry || !entry->item || !controller || !actor || actor->IsPlayerRef() ||
        !actor->IsPlayerTeammate() || static_cast<int>(entry->GetCategory()) != kOffence)
        return;
    std::string name = fmt::format("{}{}", log::NameOf(entry->item), HandTagOf(entry));
    std::scoped_lock lock(g_mutex);
    ChoiceFor(actor, controller, TacticsSeconds()).last[entry] = {std::move(name), answer};
}

void SetWaitingOnOwnCast(std::vector<std::uint32_t> followers)
{
    std::unordered_set<std::uint32_t> now(followers.begin(), followers.end());
    std::scoped_lock lock(g_mutex);
    for (const std::uint32_t id : now)
        if (!g_waiting.contains(id))
            log::ai.debug("{:08X} has a rule waiting on their own cast: their spells stand down", id);
    g_waiting = std::move(now);
}

void WatchCasts()
{
    auto *holder = RE::ScriptEventSourceHolder::GetSingleton();
    if (!holder)
    {
        log::ai.warn("no event source holder -- attack spells will not vary after a cast");
        return;
    }
    holder->AddEventSink<RE::TESSpellCastEvent>(&g_castSink);
    log::ai.info("following followers' attack choices: perks, immunities, variety");
}

void ResetAiScores()
{
    std::scoped_lock lock(g_mutex);
    g_choices.clear();
    g_waiting.clear();
}

} // namespace ft::game
