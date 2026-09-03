#include "game/Tactics.h"

#include "core/Evaluator.h"
#include "core/Vocabulary.h"
#include "game/Actions.h"
#include "game/Packages.h"
#include "game/Sensors.h"
#include "game/UI.h"
#include "game/Util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace ft::game
{
namespace
{

// --- tuning ----------------------------------------------------------------

// docs/PLAN.md 3.2 wants 150 ms.
// The TURN. Every half second the list is walked and at most one rule
// fires. There is no separate "global cooldown": the turn is the spacing
// between decisions, and the only other timers are per action.
constexpr double kTickInterval = 0.5;

// How often a follower may report *why* it did not act. Without this the log is
// seven lines per second per follower and unreadable; with it, the answer to
// "why didn't she drink" is always in the last few seconds of the file.
constexpr double kDiagnosticInterval = 2.0;

// A gap this long since we last evaluated a follower means the previous fight
// ended and this is a new one, so per-rule cooldowns are reset. A cooldown
// exists to stop a rule thrashing *within* a fight; carrying it into the next
// fight would silently suppress that fight's first heal.
constexpr double kNewFightGap = 5.0;

// How often to report measured tick cost.
constexpr double kCostReportInterval = 5.0;

constexpr std::size_t kMaxManagedFollowers = 8;

// --- state -----------------------------------------------------------------

std::atomic_bool g_enabled{true};
std::atomic_bool g_installed{false};

double g_lastTick = -1.0e9;

// Last reported follower count, so a change is logged once rather than every
// tick. Without this there is no way to tell a tick that is running and finding
// nobody from a tick that is not running at all -- both are silent, and the
// difference is "your setup script did not apply" versus "the mod is broken".
// -1 so the first report always fires, including the zero case.
int g_lastFollowerCount = -1;

struct FollowerState
{
    ft::EvalContext eval;
    double lastEvaluatedAt{-1.0e9};
    double lastDiagnosticAt{-1.0e9};
};

std::unordered_map<ft::ActorId, FollowerState> g_followers;

// The last evaluation for each follower, kept for the UI.
//
// Written on the game thread by the tick, read on the render thread by the UI,
// so it is guarded. The lock is held only for the copy in or out -- never
// across rendering, and never across BuildSnapshot.
// Only the exceptions are stored, so a follower we have never seen -- or a new
// one -- defaults to enabled without needing an entry.
std::mutex g_disabledMutex;
std::unordered_set<ft::ActorId> g_disabledFollowers;

// Followers currently in bleedout, so the transition is logged once rather
// than every tick.
std::unordered_set<ft::ActorId> g_bleedingOut;

// Per-follower rules. Absent means "has not been edited", and the default set
// is handed out instead -- so a new follower costs nothing until someone
// actually changes something.
std::mutex g_rulesMutex;
std::unordered_map<ft::ActorId, ft::RuleSet> g_ruleSets;

std::mutex g_viewMutex;
std::vector<FollowerView> g_view;

// Per-evaluation cost, in microseconds. docs/PLAN.md 3.2 sets a budget -- total
// tick cost across 8 followers under 0.5 ms/frame amortised -- and insists it be
// measured rather than assumed. This is that measurement. It is also how we will
// know whether the inventory scan needs caching, before building a cache for it.
struct TickCost
{
    double totalUs{0.0};
    double maxUs{0.0};
    std::uint64_t samples{0};

    void Add(double us)
    {
        totalUs += us;
        maxUs = std::max(maxUs, us);
        ++samples;
    }
    [[nodiscard]] double AvgUs() const
    {
        return samples ? totalUs / static_cast<double>(samples) : 0.0;
    }
    void Reset()
    {
        totalUs = 0.0;
        maxUs = 0.0;
        samples = 0;
    }
};

TickCost g_cost;
double g_lastCostReport = -1.0e9;

// --- the hardcoded Phase 1 rule --------------------------------------------

const ft::RuleSet &DefaultRuleSetImpl()
{
    static const ft::RuleSet rules = [] {
        ft::RuleSet rs;
        rs.name = "phase1-spike";

        ft::Rule heal;
        heal.label = "emergency heal";
        heal.subject = ft::SubjectKind::Self;
        heal.predicate = ft::PredicateKind::HealthPctBelow;
        heal.conditionArg = 0.5f;
        heal.actionTarget = ft::ActionTargetKind::ConditionSubject;
        heal.action = ft::ActionKind::DrinkHealthPotion;
        rs.rules.push_back(heal);

        return rs;
    }();
    return rules;
}

// Only the actions Phase 1 actually implements are advertised as supported. The
// engine then reports Verdict::Unsupported for anything else instead of firing
// a rule that Actions::Execute would silently drop.
ft::Capabilities RuntimeCapabilities(const RE::Actor *actor)
{
    ft::Capabilities caps; // all false
    caps.supported[static_cast<std::size_t>(ft::ActionKind::DrinkHealthPotion)] = true;
    caps.supported[static_cast<std::size_t>(ft::ActionKind::DrinkMagickaPotion)] = true;
    caps.supported[static_cast<std::size_t>(ft::ActionKind::DrinkStaminaPotion)] = true;
    caps.supported[static_cast<std::size_t>(ft::ActionKind::DrinkPotion)] = true;
    caps.supported[static_cast<std::size_t>(ft::ActionKind::EquipSpell)] = true;

    // Casting needs the ESL. Without it the action reports Unsupported and the
    // panel greys it out, which is a truthful "not available here" rather than
    // a rule that silently never fires.
    caps.supported[static_cast<std::size_t>(ft::ActionKind::CastSpell)] = PackagesAvailable();

    // Transient, unlike the line above: every slot mid-cast means a cast rule
    // is skipped for THIS evaluation only, with no cooldown spent, and the
    // next rule down gets its turn.
    caps.busy[static_cast<std::size_t>(ft::ActionKind::CastSpell)] =
        PackagesAvailable() && (!HasFreeSlot() || IsMidCast(actor));
    return caps;
}

// --- registry ---------------------------------------------------------------

// Who is under tactics control, right now.
//
// IsPlayerTeammate() reads the kPlayerTeammate bool bit (1 << 26), which every
// follower framework -- NFF, AFT, EFF -- sets, so this integrates with all of
// them for free and depends on none.
//
// IsInCombat() is the combat gate, and it is deliberately the ONLY one: tactics
// are a combat system, so out of combat this loop finds nobody and no snapshot
// is ever built.
//
// An earlier version cached combat state from TESCombatEvent to skip this loop.
// That was a mistake worth recording: the loop is a handle deref and two flag
// reads per nearby actor, while the cache needed a mutex and a reconciliation
// pass to survive missed events -- loading a save mid-fight fires no combat
// event, so the cache would say "nobody is fighting" forever. A lot of
// machinery, and a correctness hazard, to avoid a few microseconds. The
// expensive part of a tick is BuildSnapshot's inventory scan, and this gates
// that already.
std::vector<RE::Actor *> CollectManagedFollowers()
{
    std::vector<RE::Actor *> followers;

    auto *processLists = RE::ProcessLists::GetSingleton();
    if (!processLists)
        return followers;

    // High actors only: the fully simulated ones near the player.
    //
    // Note this does NOT filter on combat. Combat decides whether a follower is
    // EVALUATED, not whether they exist -- an earlier version conflated the two
    // and the panel stayed empty until a fight started, which is exactly when
    // you cannot calmly read it. Rules are authored before the fight.
    for (auto &handle : processLists->highActorHandles)
    {
        auto actor = handle.get();
        RE::Actor *raw = actor ? actor.get() : nullptr;
        if (!raw || raw->IsDead() || !raw->IsPlayerTeammate())
            continue;

        followers.push_back(raw);
        if (followers.size() >= kMaxManagedFollowers)
            break;
    }

    return followers;
}

// --- per-follower evaluation -------------------------------------------------

void LogDiagnostic(RE::Actor *actor, const ft::Snapshot &snap, const ft::RuleSet &rules, const ft::Trace &trace)
{
    logger::info("{} health {:.0f}/{:.0f} ({:.0f}%) combat={} potions={}", Describe(actor), snap.health.current,
                 snap.health.max, snap.health.Pct() * 100.0, snap.inCombat, snap.potions.healthCount);

    for (std::size_t i = 0; i < trace.size(); ++i)
    {
        const auto &rule = rules.rules[i];
        logger::info("    rule {} \"{}\" [{}]: {}", i, rule.label, ft::WireName(rule.action),
                     ft::Explain(trace[i], rule.action));
    }
}

ft::Stat ReadStatFor(RE::Actor *actor, RE::ActorValue av)
{
    auto *owner = actor->AsActorValueOwner();
    if (!owner)
        return {};
    return ft::Stat{owner->GetActorValue(av), owner->GetPermanentActorValue(av)};
}

// What the panel has pinned on each follower. Written by the request task,
// read by the tick; both on the game thread, but the lock costs nothing and
// keeps the next writer honest.
std::mutex g_pinMutex;
// What the panel has pinned on each follower: the form, and the hands it is
// pinned to (None for armour and ammunition). Written by the request task,
// read by the tick; both on the game thread, but the lock costs nothing and
// keeps the next writer honest.
std::unordered_map<ft::ActorId, std::unordered_map<std::uint32_t, Hand>> g_pins;

bool Overlap(Hand a, Hand b)
{
    return (static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b)) != 0;
}

// The hand's equip slot record, by FormID: LeftHand 013F43, RightHand
// 013F42 in Skyrim.esm. Not through the default object table, which did
// not answer for these on this game (01:29): a null slot here means "the
// default", and the default is the right hand -- which is where every
// left-hand dagger went (01:51).
const RE::BGSEquipSlot *HandSlot(Hand hand)
{
    return RE::TESForm::LookupByID<RE::BGSEquipSlot>(hand == Hand::Left ? 0x00013F43 : 0x00013F42);
}

// The hands a form takes when pinned, given the hand asked for.
Hand HandsFor(RE::TESForm *form, Hand requested)
{
    const Hand one = requested == Hand::Left ? Hand::Left : Hand::Right;
    if (auto *weapon = form->As<RE::TESObjectWEAP>())
    {
        if (weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() || weapon->IsCrossbow())
            return Hand::Both;
        return one;
    }
    if (auto *spell = form->As<RE::SpellItem>())
    {
        if (spell->IsTwoHanded())
            return Hand::Both;
        // A one-hand-only record (the NPC variants) goes to its hand
        // whatever was asked. By FormID, as Magic.cpp explains: RightHand is
        // 013F42 and LeftHand 013F43 in Skyrim.esm.
        if (const auto *slot = spell->GetEquipSlot())
        {
            if (slot->GetFormID() == 0x00013F43)
                return Hand::Left;
            if (slot->GetFormID() == 0x00013F42)
                return Hand::Right;
        }
        return one;
    }
    if (auto *armor = form->As<RE::TESObjectARMO>())
        return armor->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kShield) ? Hand::Left : Hand::None;
    if (form->Is(RE::FormType::Light))
        return Hand::Left;
    return Hand::None;
}

// Is the form in those hands right now?
bool EquippedIn(RE::Actor *actor, RE::TESForm *form, Hand hands)
{
    if (auto *spell = form->As<RE::SpellItem>())
    {
        const auto &data = actor->GetActorRuntimeData();
        const bool left = data.selectedSpells[RE::Actor::SlotTypes::kLeftHand] == spell;
        const bool right = data.selectedSpells[RE::Actor::SlotTypes::kRightHand] == spell;
        return (!Overlap(hands, Hand::Left) || left) && (!Overlap(hands, Hand::Right) || right);
    }
    // A two-hander sits in the right hand; the left reports it too, or
    // nothing. Asking the right is enough.
    if (hands == Hand::Both)
        return actor->GetEquippedObject(false) == form;
    return actor->GetEquippedObject(hands == Hand::Left) == form;
}

// Put a pinned form on, in its hands. `now` clears the engine's queue flag:
// queued -- the potion path's shape, right in a fight -- the change waits
// for the actor's next update, and an actor gets no update while the clock
// is frozen, so a click in the panel showed nothing until the panel closed
// (20:26, Marcurio's boots). The click path takes `now`; the tick, with
// time running, keeps the queue. Items go on with the prevent-removal flag,
// the pin; a spell has no such flag.
void EquipPinned(RE::Actor *actor, RE::TESForm *form, Hand hands, bool now)
{
    auto *manager = RE::ActorEquipManager::GetSingleton();
    if (!manager)
        return;
    if (auto *spell = form->As<RE::SpellItem>())
    {
        for (const Hand hand : {Hand::Left, Hand::Right})
        {
            if (Overlap(hands, hand))
                manager->EquipSpell(actor, spell, HandSlot(hand));
        }
        return;
    }
    auto *object = form->As<RE::TESBoundObject>();
    if (!object)
        return;
    // A one-handed weapon goes to the hand asked for; everything else finds
    // its own slot.
    const RE::BGSEquipSlot *slot = nullptr;
    if (object->Is(RE::FormType::Weapon) && hands != Hand::Both && hands != Hand::None)
        slot = HandSlot(hands);
    manager->EquipObject(actor, object, nullptr, 1, slot, !now, true, false, false);
}

// Equip an item WITHOUT the pin, for letting go of one while it stays on.
void EquipPlain(RE::Actor *actor, RE::TESBoundObject *object, Hand hands)
{
    auto *manager = RE::ActorEquipManager::GetSingleton();
    if (!manager)
        return;
    const RE::BGSEquipSlot *slot = nullptr;
    if (object->Is(RE::FormType::Weapon) && hands != Hand::Both && hands != Hand::None)
        slot = HandSlot(hands);
    manager->EquipObject(actor, object, nullptr, 1, slot, false, false, false, false);
}

// Take a form off.
//
// Items go through the equip manager WITHOUT the prevent-equip flag: the
// Creation Kit wiki notes that flag does nothing for weapons on an NPC and
// works only too well for ammunition, leaving an archer holding a bow she
// cannot use. A spell has no unequip in CommonLibSSE 3.7.0 or in SKSE; the
// engine's is the Papyrus native Actor.UnequipSpell(spell, source), 0 for
// the left hand and 1 for the right, so it is dispatched to the script VM,
// which runs it on the game thread a frame later.
void UnequipForm(RE::Actor *actor, RE::TESForm *form, Hand hands, bool now)
{
    if (auto *spell = form->As<RE::SpellItem>())
    {
        auto *vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        auto *policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
        if (!policy)
            return;
        const auto handle = policy->GetHandleForObject(actor->GetFormType(), actor);
        for (const Hand hand : {Hand::Left, Hand::Right})
        {
            if (!Overlap(hands, hand))
                continue;
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;
            vm->DispatchMethodCall2(
                handle, "Actor", "UnequipSpell",
                RE::MakeFunctionArguments(std::move(spell), static_cast<std::int32_t>(hand == Hand::Left ? 0 : 1)),
                result);
        }
        return;
    }
    if (auto *object = form->As<RE::TESBoundObject>())
    {
        // A one-handed weapon comes out of the hand named; anything else
        // out of wherever it is.
        const RE::BGSEquipSlot *slot = nullptr;
        if (object->Is(RE::FormType::Weapon) && (hands == Hand::Left || hands == Hand::Right))
            slot = HandSlot(hands);
        if (auto *manager = RE::ActorEquipManager::GetSingleton())
            manager->UnequipObject(actor, object, nullptr, 1, slot, !now, false, false, false, nullptr);
    }
}

// Whether a left-hand weapon pin also gives her a combat style that allows
// dual wielding. Off: a trial of what the unmodified style does with the
// weapon (2026-09-03).
constexpr bool kDualWieldOnLeftPin = false;

// Let her dual wield.
//
// csHumanMagic, Marcurio's style, and most vanilla styles do not allow it,
// and the AI of an actor whose style forbids it takes a left-hand weapon
// straight off again -- the loop seen with the dagger (00:26), the watchdog
// putting it back every half second. The style is shared by every mage in
// the game, so it is not edited in place: she gets a runtime copy with the
// flag set, on her record and on her live combat controller if she has
// one. Two flags are set because CommonLibSSE names the bit in two places
// (the DATA flags and the record header) and which the engine reads is
// unverified. Not saved: redone on every left-hand pin, gone with the
// session, like the pins.
void AllowDualWield(RE::Actor *actor)
{
    auto *npc = actor->GetActorBase();
    auto *style = npc ? npc->GetCombatStyle() : nullptr;
    if (!style)
        return;
    if (style->flags.all(RE::TESCombatStyle::FLAG::kAllowDualWielding))
    {
        logger::info("{} combat style {:08X} already allows dual wielding", Describe(actor), style->GetFormID());
        return;
    }

    auto *copy = style->CreateDuplicateForm(true, nullptr);
    auto *ours = copy ? copy->As<RE::TESCombatStyle>() : nullptr;
    if (!ours)
    {
        logger::warn("{} cannot be allowed to dual wield: combat style {:08X} would not duplicate", Describe(actor),
                     style->GetFormID());
        return;
    }
    // CreateDuplicateForm gives a NEW combat style at the engine's defaults
    // -- offensive 0.24, every score 1 -- not a copy of hers (01:55, the
    // Combat Style tab on the copy). The data is five plain structs and the
    // flags, so it is copied by hand.
    ours->generalData = style->generalData;
    ours->meleeData = style->meleeData;
    ours->closeRangeData = style->closeRangeData;
    ours->longRangeData = style->longRangeData;
    ours->flightData = style->flightData;
    ours->flags = style->flags;
    ours->flags.set(RE::TESCombatStyle::FLAG::kAllowDualWielding);
    ours->formFlags |= RE::TESCombatStyle::RecordFlags::kAllowDualWielding;
    npc->SetCombatStyle(ours);
    if (auto *controller = actor->GetActorRuntimeData().combatController)
        controller->combatStyle = ours;
    logger::info("{} allowed to dual wield: combat style {:08X} copied as {:08X} with the flag set", Describe(actor),
                 style->GetFormID(), ours->GetFormID());
}

// Does pinning `incoming` in `hands` mean releasing `held`, pinned in
// `heldHands`? Hands that overlap; armour on shared body slots;
// ammunition against ammunition. Anything else does not compete.
bool Conflicts(RE::TESForm *incoming, Hand hands, RE::TESForm *held, Hand heldHands)
{
    if (hands != Hand::None && heldHands != Hand::None)
        return Overlap(hands, heldHands);
    auto *a = incoming->As<RE::TESObjectARMO>();
    auto *b = held->As<RE::TESObjectARMO>();
    if (a && b)
        return (static_cast<std::uint32_t>(a->GetSlotMask()) & static_cast<std::uint32_t>(b->GetSlotMask())) != 0U;
    return incoming->Is(RE::FormType::Ammo) && held->Is(RE::FormType::Ammo);
}

// A pinned item is locked against the engine's own swap -- the Creation Kit
// wiki: prevent-removal "does prevent removal when using EquipItem(OtherItem)"
// -- so before something new goes on, whatever it displaces has to be
// unpinned and taken off by us, or the equip silently does nothing. That is
// what happened with iron armour pinned and robes clicked.
void ReleaseConflictingPins(RE::Actor *actor, std::unordered_map<std::uint32_t, Hand> &pins, RE::TESForm *incoming,
                            Hand hands)
{
    for (auto it = pins.begin(); it != pins.end();)
    {
        auto *held = RE::TESForm::LookupByID(it->first);
        if (held && held != incoming && Conflicts(incoming, hands, held, it->second))
        {
            logger::info("{} unpinning {} to make room", Describe(actor), held->GetName() ? held->GetName() : "?");
            UnequipForm(actor, held, it->second, true);
            it = pins.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

// Mark the scanned items and spells that are pinned, for the panel's hand
// and Equipped cells, and drop any pin for something the scans did not
// find: sold, dropped, the last arrow shot. A set lookup per entry and
// nothing more; the watchdog catches the same case on its own, but there
// is no reason to leave a dead pin for it to find.
void MarkPins(ft::ActorId id, std::vector<InventoryItem> &items, std::vector<MagicEntry> &magic)
{
    std::scoped_lock lock(g_pinMutex);
    const auto it = g_pins.find(id);
    if (it == g_pins.end() || it->second.empty())
        return;
    auto &pins = it->second;

    std::unordered_set<std::uint32_t> present;
    for (auto &item : items)
    {
        present.insert(item.form);
        if (const auto pin = pins.find(item.form); pin != pins.end())
        {
            item.pinned = pin->second == Hand::None;
            item.pinnedLeft = Overlap(pin->second, Hand::Left);
            item.pinnedRight = Overlap(pin->second, Hand::Right);
        }
    }
    for (auto &entry : magic)
    {
        present.insert(entry.form);
        if (const auto pin = pins.find(entry.form); pin != pins.end())
        {
            entry.pinnedLeft = Overlap(pin->second, Hand::Left);
            entry.pinnedRight = Overlap(pin->second, Hand::Right);
        }
    }
    std::erase_if(pins, [&](const auto &pin) { return !present.contains(pin.first); });
}

// The watchdog: put back any pinned form the game has taken off, and forget
// pins for items no longer carried.
//
// Its own pass, not a rider on the inventory scan, and gated on the pin map:
// with nothing pinned it costs a lock and a look at an empty map. With pins
// it asks the engine for each pinned item's entry alone -- a walk of pointer
// compares, no names, no strings -- so a follower with two pins costs two
// lookups a tick, not a sweep of her bag.
//
// In a fight her hands are the combat AI's, and the rules': a mage with a
// pinned dagger wants that hand for a spell, and putting the dagger back
// every half second had the two trading blows -- the flicker seen with
// Marcurio. So anything pinned to a hand -- weapon, spell, shield, torch --
// is what she carries out of combat and starts a fight with; once it is
// over, it goes back. Armour and ammunition are contested by nothing and
// hold throughout.
void EnforcePins(const std::vector<RE::Actor *> &followers)
{
    std::scoped_lock lock(g_pinMutex);
    if (g_pins.empty())
        return;

    for (auto *actor : followers)
    {
        const auto it = g_pins.find(actor->GetFormID());
        if (it == g_pins.end())
            continue;
        auto &pins = it->second;
        const bool fighting = actor->IsInCombat();

        for (auto pin = pins.begin(); pin != pins.end();)
        {
            auto *form = RE::TESForm::LookupByID(pin->first);
            if (!form)
            {
                pin = pins.erase(pin);
                continue;
            }
            const Hand hands = pin->second;

            // Spells first: a SpellItem is a bound object too, and the
            // inventory branch dropped every spell pin as "no longer
            // carried" (00:26, Close Wounds).
            if (form->Is(RE::FormType::Spell))
            {
                if (!fighting && !EquippedIn(actor, form, hands))
                {
                    logger::info("{} put away pinned {} -- readying it again", Describe(actor),
                                 form->GetName() ? form->GetName() : "?");
                    EquipPinned(actor, form, hands, false);
                }
            }
            else if (auto *object = form->As<RE::TESBoundObject>())
            {
                auto inventory =
                    actor->GetInventory([object](RE::TESBoundObject &candidate) { return &candidate == object; });
                const auto found = inventory.find(object);
                const bool carried = found != inventory.end() && found->second.first > 0;
                if (!carried)
                {
                    logger::info("{} no longer carries {} -- pin dropped", Describe(actor),
                                 object->GetName() ? object->GetName() : "?");
                    pin = pins.erase(pin);
                    continue;
                }
                const bool on = hands == Hand::None ? (found->second.second && found->second.second->IsWorn())
                                                    : EquippedIn(actor, object, hands);
                if (!on && !(fighting && hands != Hand::None))
                {
                    logger::info("{} took off pinned {} -- putting it back on", Describe(actor),
                                 object->GetName() ? object->GetName() : "?");
                    EquipPinned(actor, object, hands, false);
                }
            }
            ++pin;
        }
    }

    std::erase_if(g_pins, [](const auto &entry) { return entry.second.empty(); });
}

// Level and carry weight, for the panel. Not rule inputs -- three cheap reads,
// done on both the in-combat and idle paths so the panel does not go blank when
// a fight ends.
void FillDisplayFields(RE::Actor *actor, FollowerView &v)
{
    v.level = actor->GetLevel();
    v.carriedWeight = actor->GetWeightInContainer();
    if (auto *owner = actor->AsActorValueOwner())
        v.carryCapacity = owner->GetActorValue(RE::ActorValue::kCarryWeight);

    // Scanned on the idle path too, so the spell menu is populated while rules
    // are being written -- which is the only time anyone opens it. A follower's
    // spell list changes rarely, but it does change (the console addspell that
    // set this test up is exactly such a change), so it is re-read rather than
    // cached until something invalidates it.
    v.spells = ScanCastableSpells(actor);
    v.potions = ScanCarriedPotions(actor);
    v.sheet = BuildCharacterSheet(actor);
    v.skills = BuildSkillSheet(actor);
    v.inventory = ScanInventory(actor);
    v.magic = ScanMagic(actor);
    MarkPins(v.id, v.inventory, v.magic);
    v.combatStyle = BuildCombatStyleSheet(actor);
}

void PublishOne(FollowerView v)
{
    std::scoped_lock lock(g_viewMutex);
    for (auto &existing : g_view)
    {
        if (existing.id == v.id)
        {
            existing = std::move(v);
            return;
        }
    }
    g_view.push_back(std::move(v));
}

// Out of combat: read what is cheap and skip what is not.
//
// Three actor-value reads and a couple of flags -- no inventory scan, no
// evaluation. That keeps "tactics only run in combat" true while still letting
// the panel show who is under control and what shape they are in.
void PublishIdle(RE::Actor *actor, double now, bool inCombat)
{
    ft::Snapshot snapshot;
    snapshot.self = actor->GetFormID();
    snapshot.now = now;
    snapshot.health = ReadStatFor(actor, RE::ActorValue::kHealth);
    snapshot.magicka = ReadStatFor(actor, RE::ActorValue::kMagicka);
    snapshot.stamina = ReadStatFor(actor, RE::ActorValue::kStamina);

    FollowerView v;
    v.id = snapshot.self;
    v.name = DisplayNameOf(actor);
    v.snapshot = snapshot;
    v.lastEvaluatedAt = now;
    v.evaluated = false;
    v.inCombat = inCombat;
    v.tacticsEnabled = IsFollowerEnabled(v.id);
    FillDisplayFields(actor, v);
    PublishOne(std::move(v));
}

void PublishView(RE::Actor *actor, const ft::Snapshot &snapshot, const ft::Trace &trace, const ft::Decision &decision,
                 double now);

void EvaluateFollower(RE::Actor *actor, double now)
{
    const ft::ActorId id = actor->GetFormID();
    auto &state = g_followers[id];

    // A long gap since the last evaluation means a different fight.
    if ((now - state.lastEvaluatedAt) > kNewFightGap)
    {
        state.eval = {};
        logger::info("{} entered combat -- tactics engaged", Describe(actor));
    }
    state.lastEvaluatedAt = now;
    state.eval.caps = RuntimeCapabilities(actor);

    const auto started = std::chrono::steady_clock::now();

    PotionChoice choice;
    const ft::Snapshot snapshot = BuildSnapshot(actor, now, choice);

    // This follower's own rules, not a shared static -- the whole point of
    // making them per-follower.
    const ft::RuleSet rules = GetRules(id);
    ft::Trace trace;
    const ft::Decision decision = ft::Evaluate(rules, snapshot, state.eval, &trace);

    PublishView(actor, snapshot, trace, decision, now);

    g_cost.Add(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());

    if (decision.Fired())
    {
        const auto result = Execute(decision, actor, choice);

        logger::info("{} FIRED rule {} \"{}\" [{}] -> {} [health {:.0f}/{:.0f} = {:.0f}%]", Describe(actor),
                     decision.ruleIndex, rules.rules[decision.ruleIndex].label, ft::WireName(decision.action),
                     ToString(result), snapshot.health.current, snapshot.health.max, snapshot.health.Pct() * 100.0);

        // Empirical check for whether a drunk potion leaves a lingering effect
        // we could test against, rather than relying on a fixed settle time.
        LogActiveEffects(actor, "just after firing");

        if (result != ActionResult::Performed)
        {
            // A rule that fires but does not take effect is the failure worth
            // shouting about: the engine believed it acted, and it did not.
            logger::warn("{} action did NOT take effect: {}", Describe(actor), ToString(result));
        }
        return;
    }

    if ((now - state.lastDiagnosticAt) >= kDiagnosticInterval)
    {
        state.lastDiagnosticAt = now;
        LogDiagnostic(actor, snapshot, rules, trace);
    }
}

// Replace this follower's entry in the observable view. Called for every
// follower every tick, whether or not a rule fired -- the debug column is most
// useful precisely when nothing is firing.
void PublishView(RE::Actor *actor, const ft::Snapshot &snapshot, const ft::Trace &trace, const ft::Decision &decision,
                 double now)
{
    FollowerView v;
    v.id = actor->GetFormID();
    v.name = DisplayNameOf(actor);
    v.snapshot = snapshot;
    v.trace = trace;
    v.decision = decision;
    v.lastEvaluatedAt = now;
    v.evaluated = true;
    v.inCombat = true;
    v.tacticsEnabled = true;
    FillDisplayFields(actor, v);
    PublishOne(std::move(v));
}

// --- the tick ---------------------------------------------------------------

// Tick-side wrapper: same predicate, plus a line in the log when the answer
// changes.
//
// Logged on change only. This runs every tick, and a gate that stays silent
// when it works is indistinguishable from one that is not running at all --
// which is exactly how the previous version stayed hidden for a whole test
// round. It also tells us WHICH signal caught a given menu, so this can be
// narrowed later on evidence rather than on a guess.
bool EvaluationHeld()
{
    const ClockState clock = ReadClock();

    static int previous = -1;
    const int state = (clock.pausedMenu ? 1 : 0) | (clock.frozenClock ? 2 : 0);
    if (state != previous)
    {
        previous = state;
        if (state == 0)
            logger::info("tactics: time is running -- evaluating");
        else
            logger::info("tactics: time stopped ({}{}{}) -- evaluation held", clock.pausedMenu ? "paused menu" : "",
                         (clock.pausedMenu && clock.frozenClock) ? " + " : "", clock.frozenClock ? "frozen clock" : "");
    }
    return clock.stopped();
}

// Pacing lives on a separate thread; the work itself runs on the game thread via
// the task interface.
//
// It MUST be this way round. SKSE processes its task queue like this
// (skse64/Hooks_Threads.cpp):
//
//     void BSTaskPool::ProcessTasks() {
//         ...
//         while (!IsTaskQueueEmpty()) { cmd->Run(); cmd->Dispose(); }
//     }
//
// It drains until the queue is EMPTY. So a task that calls AddTask from inside
// its own Run() refills the queue faster than it drains, ProcessTasks never
// returns, and the main thread spins forever -- the game hangs on the first
// frame. A self-re-arming task is not a periodic scheduler in SKSE; it is a
// deadlock. This cost one hung startup to learn.
void Tick()
{
    // Not an early return on the tactics switch: the switch gates rule
    // EVALUATION, and the rest of this -- the views behind the Character,
    // Skills and Inventory tabs, the pin watchdog -- is not tactics and runs
    // whether or not she is being told what to do. The frozen clock still
    // holds everything, since nothing below can act on a stopped world.
    if (EvaluationHeld())
        return;

    // Game time, in real seconds: it does not advance while the game is
    // paused, so nothing below is aged by a menu.
    const double now = TacticsSeconds();
    if ((now - g_lastTick) < kTickInterval)
        return;
    g_lastTick = now;

    // No player means main menu or a load screen. Walking the process lists
    // then is pointless at best.
    if (!RE::PlayerCharacter::GetSingleton())
        return;

    const auto followers = CollectManagedFollowers();

    if (static_cast<int>(followers.size()) != g_lastFollowerCount)
    {
        g_lastFollowerCount = static_cast<int>(followers.size());
        if (followers.empty())
        {
            logger::info("tactics: 0 followers. Nobody nearby has the player-teammate flag -- "
                         "if you just ran a setup script, prid probably selected nothing.");
        }
        else
        {
            std::string names;
            for (auto *f : followers)
            {
                if (!names.empty())
                    names += ", ";
                names += Describe(f);
            }
            logger::info("tactics: {} follower(s) under control: {}", followers.size(), names);
        }
    }

    // Out of combat there is nothing to decide, so the expensive work -- the
    // inventory scan inside BuildSnapshot, and the evaluation itself -- is
    // skipped entirely. What remains is a few actor-value reads, so the panel
    // is not blank while you are standing there authoring rules.
    for (auto *follower : followers)
    {
        const bool fighting = follower->IsInCombat();

        // Both switches must be on. A follower turned off still appears in the
        // panel, and still reports whether they are fighting -- they are simply
        // not evaluated, which is what the empty Status column then says.
        // Bleeding out, nothing can be performed: no potion, no cast, and the
        // 12:20 run fired a cast rule four times at negative health. Hold
        // evaluation until she is up again, and say so once.
        const bool down = follower->AsActorState() && follower->AsActorState()->IsBleedingOut();
        const bool wasDown = g_bleedingOut.contains(follower->GetFormID());
        if (down != wasDown)
        {
            logger::info("{} {}", Describe(follower),
                         down ? "is bleeding out -- tactics held" : "is up -- tactics resume");
            if (down)
                g_bleedingOut.insert(follower->GetFormID());
            else
                g_bleedingOut.erase(follower->GetFormID());
        }

        if (g_enabled.load() && fighting && !down && IsFollowerEnabled(follower->GetFormID()))
            EvaluateFollower(follower, now);
        else
            PublishIdle(follower, now, fighting);
    }

    // Armed cast requests are withdrawn from here, whether or not anyone is
    // still fighting: a request must not outlive the moment it was made for.
    TickPackages(now, followers);

    // Pinned gear, independent of tactics. Cheap when nothing is pinned.
    EnforcePins(followers);

    // Drop anyone who is no longer a managed follower -- dismissed, dead, or out
    // of range -- so the panel reflects the present rather than a history.
    {
        std::scoped_lock lock(g_viewMutex);
        std::erase_if(g_view, [&](const FollowerView &v) {
            for (auto *f : followers)
            {
                if (f->GetFormID() == v.id)
                    return false;
            }
            return true;
        });
    }

    // Menu entries follow the views: added for a newcomer, removed for the
    // dismissed. After the erase above, so a dismissed follower is gone from
    // the views by the time this looks.
    ui::SyncFollowers();

    if (g_cost.samples > 0 && (now - g_lastCostReport) >= kCostReportInterval)
    {
        g_lastCostReport = now;
        logger::info("tactics: {} evaluations, avg {:.0f} us, max {:.0f} us  (budget: under "
                     "500 us/frame across all followers)",
                     g_cost.samples, g_cost.AvgUs(), g_cost.maxUs);
        g_cost.Reset();
    }
}

} // namespace

// Two signals, because neither alone is enough.
//
//   numPausesGame    what UI::GameIsPaused() returns, and it is nothing more
//                    than a count of registered menus carrying kPausesGame.
//                    It covers the inventory, map, journal, settings and the
//                    console. It CANNOT see our own panel: SKSE Menu Framework
//                    draws from a D3D present hook and never registers an
//                    IMenu, so it never moves that counter no matter what
//                    FreezeTimeOnMenu says. An earlier gate checked only this
//                    and let the panel straight through -- for a whole test
//                    round, because it also went unlogged.
//
//   Main::freezeTime the clock itself, which is what the framework sets when
//                    FreezeTimeOnMenu = true.
//
// Whether a pausing menu ALSO sets freezeTime is not established, so the two
// are OR-ed rather than one being assumed to imply the other. Both are a
// pointer dereference; there is nothing to win by guessing.
//
// Asking about the clock rather than about panel-is-open also gets
// FreezeTimeOnMenu = false right for free: with the freeze off, time keeps
// running and so do rules, so the panel shows live state rather than a still
// frame. That is the whole point of that setting.
ClockState ReadClock()
{
    auto *ui = RE::UI::GetSingleton();
    auto *main = RE::Main::GetSingleton();
    return ClockState{ui && ui->GameIsPaused(), main && main->freezeTime};
}

ft::RuleSet GetRules(ft::ActorId id)
{
    std::scoped_lock lock(g_rulesMutex);
    const auto it = g_ruleSets.find(id);
    return it == g_ruleSets.end() ? DefaultRuleSet() : it->second;
}

void RequestWear(ft::ActorId id, std::uint32_t form, WearRequest request, Hand hand)
{
    auto *task = SKSE::GetTaskInterface();
    if (!task)
        return;
    // Queued to the game thread and run there once. The panel is open while
    // this is clicked, and with FreezeTimeOnMenu the tick is held, so the
    // task also republishes her view: the cell answers now rather than when
    // the panel closes.
    task->AddTask([id, form, request, hand] {
        auto *actor = RE::TESForm::LookupByID<RE::Actor>(id);
        auto *thing = RE::TESForm::LookupByID(form);
        if (!actor || !thing)
            return;

        Hand hands = HandsFor(thing, hand);
        {
            std::scoped_lock lock(g_pinMutex);
            auto &pins = g_pins[id];
            if (request == WearRequest::Pin)
            {
                ReleaseConflictingPins(actor, pins, thing, hands);
                pins[form] = hands;
            }
            else
            {
                if (const auto pin = pins.find(form); pin != pins.end())
                    hands = pin->second;
                pins.erase(form);
            }
        }

        const char *name = thing->GetName() ? thing->GetName() : "?";
        switch (request)
        {
        case WearRequest::Pin:
            logger::info("{} told to ready {} (pinned)", Describe(actor), name);
            // Off for now, to see what her own style does with a left-hand
            // weapon; the copy stays available for the combat-style work.
            if (kDualWieldOnLeftPin && thing->Is(RE::FormType::Weapon) && hands == Hand::Left)
                AllowDualWield(actor);
            // One weapon cannot be in both hands. Asked to move her only copy
            // to the other hand, take it out of the first; otherwise the
            // engine's equip, finding none free, conjures a second (02:05,
            // the doubled dagger). Two in the bag may go one per hand.
            if (thing->Is(RE::FormType::Weapon) && (hands == Hand::Left || hands == Hand::Right))
            {
                const Hand other = hands == Hand::Left ? Hand::Right : Hand::Left;
                if (EquippedIn(actor, thing, other))
                {
                    auto *object = thing->As<RE::TESBoundObject>();
                    auto inventory = actor->GetInventory([object](RE::TESBoundObject &c) { return &c == object; });
                    const auto found = inventory.find(object);
                    const auto count = found != inventory.end() ? found->second.first : 0;
                    if (count < 2)
                        UnequipForm(actor, thing, other, true);
                }
            }
            EquipPinned(actor, thing, hands, true);
            // Which hand a weapon or spell lands in is the AI's call as much
            // as ours: say what was asked and where it went, so the rule can
            // be read off the log.
            if (thing->Is(RE::FormType::Weapon))
            {
                logger::info("{} weapon {} asked {} -- now left {} right {}", Describe(actor), name,
                             static_cast<int>(hands), actor->GetEquippedObject(true) == thing,
                             actor->GetEquippedObject(false) == thing);
            }
            if (auto *spell = thing->As<RE::SpellItem>())
            {
                const auto *slot = spell->GetEquipSlot();
                const auto &data = actor->GetActorRuntimeData();
                logger::info("{} spell {} asked {} -- record slot {:06X} -- now left {} right {}", Describe(actor),
                             name, static_cast<int>(hands), slot ? slot->GetFormID() : 0,
                             data.selectedSpells[RE::Actor::SlotTypes::kLeftHand] == spell,
                             data.selectedSpells[RE::Actor::SlotTypes::kRightHand] == spell);
            }
            break;
        case WearRequest::Unpin:
            // An item's lock lives on the worn item, and the engine offers no
            // way to lift it in place: off, then on again without the flag.
            // A spell has no lock; forgetting the pin is the whole of it.
            logger::info("{} told to keep {} but not held to it", Describe(actor), name);
            // Spell first: a SpellItem is a bound object too, and the item
            // branch took a spell off and "put it back" with an item equip,
            // which left it off (01:47, Chain Lightning).
            if (thing->Is(RE::FormType::Spell))
                break;
            if (auto *object = thing->As<RE::TESBoundObject>())
            {
                UnequipForm(actor, object, hands, true);
                EquipPlain(actor, object, hands);
            }
            break;
        case WearRequest::TakeOff:
            logger::info("{} told to put away {}", Describe(actor), name);
            UnequipForm(actor, thing, hands, true);
            break;
        }

        // Redraw her now. The Creation Kit wiki, on EquipItem: armour
        // equipped while a menu holds the actor "will not be visible ...
        // until the dialogue is ended" unless the model is refreshed
        // straight after -- which is this call, the one SKSE's
        // QueueNiNodeUpdate wraps.
        actor->Update3DModel();

        // NOT applied here: the item's enchantment. The equip path applies it
        // on the actor's next update, which the frozen clock withholds, so
        // the Skills tab shows the change only once the panel has closed and
        // time has run. Applying it here as well (UpdateArmorAbility) put
        // robes of Destruction at -34% instead of -17%: the engine's own
        // application still came, on top. Deferred it stays.

        PublishIdle(actor, TacticsSeconds(), actor->IsInCombat());
    });
}

void SetRules(ft::ActorId id, ft::RuleSet rules)
{
    std::scoped_lock lock(g_rulesMutex);
    g_ruleSets[id] = std::move(rules);
}

const ft::RuleSet &DefaultRuleSet()
{
    return DefaultRuleSetImpl();
}

std::vector<FollowerView> ObserveFollowers()
{
    std::scoped_lock lock(g_viewMutex);
    return g_view;
}

CostStats ObserveCost()
{
    return CostStats{g_cost.AvgUs(), g_cost.maxUs, g_cost.samples};
}

const ft::RuleSet &ActiveRuleSet()
{
    return DefaultRuleSet();
}

void Install()
{
    if (g_installed.exchange(true))
        return;

    logger::info("tactics: tick {:.0f} ms, combat only, max {} followers", kTickInterval * 1000.0,
                 kMaxManagedFollowers);
    logger::info("tactics: default rule set -- self health below 50% -> drink the best health potion");

    // Detached on purpose: Skyrim never unloads SKSE plugins, and joining a
    // sleeping thread during process teardown is a good way to hang on exit.
    std::thread([] {
        while (g_installed.load())
        {
            std::this_thread::sleep_for(std::chrono::duration<double>(kTickInterval));
            if (auto *task = SKSE::GetTaskInterface())
                task->AddTask([] { Tick(); });
        }
    }).detach();
}

void SetFollowerEnabled(ft::ActorId id, bool enabled)
{
    std::scoped_lock lock(g_disabledMutex);
    if (enabled)
        g_disabledFollowers.erase(id);
    else
        g_disabledFollowers.insert(id);
}

bool IsFollowerEnabled(ft::ActorId id)
{
    std::scoped_lock lock(g_disabledMutex);
    return !g_disabledFollowers.contains(id);
}

void SetEnabled(bool enabled)
{
    g_enabled.store(enabled);
    logger::info("tactics: {}", enabled ? "enabled" : "disabled");
}

bool IsEnabled()
{
    return g_enabled.load();
}

} // namespace ft::game
