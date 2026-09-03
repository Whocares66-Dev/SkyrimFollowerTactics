// Pins: what stays in a hand, and how the promise is kept. The rules are
// core/Loadout.cpp; this is where they meet the engine. Everything here runs
// on the game thread -- the tick, or a task the panel queued.

#include "game/Pins.h"

#include "game/Tactics.h"
#include "game/Util.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ft::game
{
namespace
{

// What the panel has pinned on each follower. Written by the request task,
// read by the tick; both on the game thread, but the lock costs nothing and
// keeps the next writer honest.
std::recursive_mutex g_pinMutex;
// What the panel has pinned on each follower: the form, and the hands it is
// pinned to (None for armour and ammunition). Written by the request task,
// read by the tick; both on the game thread, but the lock costs nothing and
// keeps the next writer honest.
std::unordered_map<ft::ActorId, std::unordered_map<std::uint32_t, Hand>> g_pins;

// The hand's equip slot record, by FormID: LeftHand 013F43, RightHand
// 013F42 in Skyrim.esm. Not through the default object table, which did
// not answer for these on this game (01:29): a null slot here means "the
// default", and the default is the right hand -- which is where every
// left-hand dagger went (01:51).
const RE::BGSEquipSlot *HandSlot(Hand hand)
{
    return RE::TESForm::LookupByID<RE::BGSEquipSlot>(hand == Hand::Left ? 0x00013F43 : 0x00013F42);
}

// The hand a slot record names: RightHand 013F42, LeftHand 013F43,
// BothHands 013F45. EitherHand (013F44) and no record are None.
Hand SlotHand(const RE::BGSEquipSlot *slot)
{
    switch (slot ? slot->GetFormID() : 0)
    {
    case 0x00013F42:
        return Hand::Right;
    case 0x00013F43:
        return Hand::Left;
    case 0x00013F45:
        return Hand::Both;
    default:
        return Hand::None;
    }
}

// For the log: which hand an entry of the AI's list would go into.
const char *HandTag(Hand hand)
{
    switch (hand)
    {
    case Hand::Left:
        return "[L]";
    case Hand::Right:
        return "[R]";
    case Hand::Both:
        return "[LR]";
    default:
        return "";
    }
}

// The planner's description of a form: which hands its record lets it
// take, whether the combat AI would choose it, which body slots it covers.
// The ONLY place the pin rules meet a record; the rules themselves are in
// core/Loadout.cpp, where they are tested.
Holdable DescribeHoldable(RE::Actor *actor, RE::TESForm *form)
{
    Holdable thing;
    thing.form = form->GetFormID();
    if (auto *weapon = form->As<RE::TESObjectWEAP>())
    {
        const bool bothHands =
            weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() || weapon->IsCrossbow();
        thing.grip = bothHands ? Grip::Both : Grip::Either;
    }
    else if (auto *spell = form->As<RE::SpellItem>())
    {
        if (spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell)
            return thing; // a power, an ability: no hand
        // The slot records, by FormID from Skyrim.esm: RightHand 013F42,
        // LeftHand 013F43, EitherHand 013F44, BothHands 013F45. The default
        // object table did not answer for them on this game.
        const auto *slot = spell->GetEquipSlot();
        const std::uint32_t slotId = slot ? slot->GetFormID() : 0;
        thing.grip = spell->IsTwoHanded()   ? Grip::Both
                     : slotId == 0x00013F43 ? Grip::LeftOnly
                     : slotId == 0x00013F42 ? Grip::RightOnly
                                            : Grip::Either;
        // Above her skill in its school: the combat AI will not choose it.
        const auto *costliest = spell->GetCostliestEffectItem();
        const auto *effect = costliest ? costliest->baseEffect : nullptr;
        auto *owner = actor->AsActorValueOwner();
        if (effect && owner)
            thing.unusable = effect->GetMinimumSkillLevel() > owner->GetActorValue(effect->GetMagickSkill());
    }
    else if (auto *armor = form->As<RE::TESObjectARMO>())
    {
        thing.slots = static_cast<std::uint32_t>(armor->GetSlotMask());
        thing.grip = ArmorGrip(armor);
    }
    else if (form->Is(RE::FormType::Light))
    {
        thing.grip = Grip::LeftOnly;
    }
    else if (form->Is(RE::FormType::Ammo))
    {
        thing.ammo = true;
    }
    return thing;
}

// This follower's pins as the planner takes them.
// One pin as the planner takes it: the hands, and for a pin with none,
// the body slots or the quiver it holds.
Pin PlannedPin(RE::Actor *actor, std::uint32_t form, Hand hands)
{
    Pin pin;
    pin.form = form;
    pin.hands = hands;
    if (auto *thing = RE::TESForm::LookupByID(form); thing && hands == Hand::None)
    {
        const Holdable described = DescribeHoldable(actor, thing);
        pin.slots = described.slots;
        pin.ammo = described.ammo;
    }
    return pin;
}

std::vector<Pin> PinsOf(ft::ActorId id)
{
    std::scoped_lock lock(g_pinMutex);
    std::vector<Pin> out;
    auto *actor = RE::TESForm::LookupByID<RE::Actor>(id);
    if (const auto it = g_pins.find(id); actor && it != g_pins.end())
    {
        for (const auto &[form, hands] : it->second)
            out.push_back(PlannedPin(actor, form, hands));
    }
    return out;
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
// For the log: the selected spell and the hand's caster, both hands. The
// two differ while a spell equip is only half done -- the menu's equip
// sounds once at the click and once more when the panel closes (14:19),
// and this says which half plays the second.
std::string CasterState(RE::Actor *actor)
{
    const auto &data = actor->GetActorRuntimeData();
    const auto name = [](const RE::MagicItem *spell) { return spell && spell->GetName() ? spell->GetName() : "-"; };
    const auto *left = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kLeftHand);
    const auto *right = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand);
    return std::string("selected L=") + name(data.selectedSpells[RE::Actor::SlotTypes::kLeftHand]) +
           " R=" + name(data.selectedSpells[RE::Actor::SlotTypes::kRightHand]) +
           " -- caster L=" + name(left ? left->currentSpell : nullptr) +
           " R=" + name(right ? right->currentSpell : nullptr);
}

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
                EquipSpellIn(actor, spell, hand);
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
// Is the item on, in a hand or worn? The no-op check before an unequip.
bool Worn(RE::Actor *actor, RE::TESBoundObject *object, Hand hands)
{
    if (hands != Hand::None)
        return EquippedIn(actor, object, hands);
    auto inventory = actor->GetInventory([object](RE::TESBoundObject &c) { return &c == object; });
    const auto found = inventory.find(object);
    return found != inventory.end() && found->second.second && found->second.second->IsWorn();
}

// Take a spell out of a hand, or an item off. A no-op when it is not
// there, like EquipSpellIn: a spell's unequip is a Papyrus call and a
// republish, and neither is owed for a hand that was already empty.
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
            if (!Overlap(hands, hand) || !EquippedIn(actor, spell, hand))
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
        if (!Worn(actor, object, hands))
            return;
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
[[maybe_unused]] void AllowDualWield(RE::Actor *actor)
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

// A pinned item is locked against the engine's own swap -- the Creation Kit
// wiki: prevent-removal "does prevent removal when using EquipItem(OtherItem)"
// -- so before something new goes on, whatever it displaces has to be
// unpinned and taken off by us, or the equip silently does nothing. That is
// what happened with iron armour pinned and robes clicked.
void ReleaseConflictingPins(RE::Actor *actor, std::unordered_map<std::uint32_t, Hand> &pins, RE::TESForm *incoming,
                            Hand hands)
{
    const Holdable coming = DescribeHoldable(actor, incoming);
    for (auto it = pins.begin(); it != pins.end();)
    {
        auto *held = RE::TESForm::LookupByID(it->first);
        const Holdable holding = held ? DescribeHoldable(actor, held) : Holdable{};
        if (!held || held == incoming || !Conflicts(coming, hands, holding, it->second))
        {
            ++it;
            continue;
        }
        const char *name = held->GetName() ? held->GetName() : "?";
        // A pin holding both hands with one thing per hand -- two daggers
        // -- gives up only the hand asked for and keeps the other. A
        // two-hander, or a pin with no hand, goes whole.
        const Hand taken = Common(hands, it->second);
        const bool partly = holding.grip == Grip::Either && taken != Hand::None && taken != it->second;
        if (partly)
        {
            logger::info("{} unpinning {} from one hand to make room", Describe(actor), name);
            UnequipForm(actor, held, taken, true);
            it->second = Without(it->second, taken);
            ++it;
            continue;
        }
        logger::info("{} unpinning {} to make room", Describe(actor), name);
        UnequipForm(actor, held, it->second, true);
        it = pins.erase(it);
    }
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

// --- keeping the AI to the pins ------------------------------------------
//
// The combat AI chooses from a list of its own, the combat inventory it
// builds when a fight begins: spells and items together, scored, in seven
// arrays by role. It does not read her spell lists or her bag again during
// the fight -- Firebolt was cast after being removed from her record, a
// removed dagger never was. So a pin is kept by answering THAT list's
// scoring, below: an entry that would take a pinned hand scores zero when
// the AI asks. Nothing of hers changes, nothing is saved, and the engine
// discards the list when the fight ends, so there is nothing to restore.
// Two earlier ways were dropped: removing competing spells from her record
// for the life of a pin (it worked, and left her without them for every
// menu, script and mod in between), and erasing entries from the list on
// each tick (a race the AI won; it re-lists every few seconds).

// What the combat AI is choosing from: its combat inventory, seven arrays
// of scored options built for the fight. Logged once per fight, by name,
// to learn the layout -- the AI cast a spell we had removed from her lists
// (03:18), so this list, not those, is what it reads.
std::unordered_set<ft::ActorId> g_probedFights;
// Entries whose zeroed score has been logged this fight: once each.
std::unordered_set<const RE::CombatInventoryItem *> g_zeroedOnce;

// HOW A PIN IS KEPT FROM THE AI. Its list of options is a list of scored
// entries, and it asks each entry for its score every time it decides
// what to hold. That call is a virtual, so its slot in each entry class's
// table is ours: ZERO for an entry the pins keep from the AI, the class's
// own answer for everything else. Reactive and exact -- nothing is
// computed until the AI asks, and however often it re-lists its options
// (every few seconds, for the range it is at) the answer is the same.
// The classes are hooked at load from the address library's table; any
// class first seen in a list at combat start is hooked then, in case the
// table missed one. The original is kept per vtable.
using ScoreFn = float (*)(RE::CombatInventoryItem *, RE::CombatController *);
std::unordered_map<std::uintptr_t, ScoreFn> g_scoreOriginals;
constexpr std::size_t kCalculateScoreSlot = 0x0C;

// The actor whose AI this controller is, or null. The hook fires for every
// creature's AI, not only a follower's, and the attacker is found by its
// HANDLE, which the handle table validates: the cached pointer beside it
// read as the value 1 for a cave bear's controller and crashed the game
// twice (15:04, 15:12). A controller whose inventory does not point back
// at it is not the shape we expect, and is left alone.
RE::NiPointer<RE::Actor> AttackerOf(RE::CombatController *controller)
{
    if (!controller || !controller->inventory || controller->inventory->parentController != controller)
        return {};
    return controller->attackerHandle.get();
}

bool ShadowedEntry(RE::CombatInventoryItem *entry, RE::Actor *actor)
{
    if (!entry || !entry->item || !actor)
        return false;
    const std::vector<Pin> pins = PinsOf(actor->GetFormID());
    if (pins.empty())
        return false;
    return KeptFromAI(pins, DescribeHoldable(actor, entry->item), SlotHand(entry->itemSlot.equipSlot));
}

float ScoreHook(RE::CombatInventoryItem *self, RE::CombatController *controller)
{
    const auto vtable = *reinterpret_cast<const std::uintptr_t *>(self);
    const auto original = g_scoreOriginals.find(vtable);
    const float score = original != g_scoreOriginals.end() ? original->second(self, controller) : 0.0f;
    const RE::NiPointer<RE::Actor> actor = AttackerOf(controller);
    if (!ShadowedEntry(self, actor.get()))
        return score;
    if (g_zeroedOnce.insert(self).second)
    {
        logger::info("{} AI asked the score of {}{}: {:.2f}, answered 0 (pinned against)", Describe(actor.get()),
                     self->item->GetName() ? self->item->GetName() : "?", HandTag(SlotHand(self->itemSlot.equipSlot)),
                     score);
    }
    return 0.0f;
}

void WatchScoresIn(std::uintptr_t vtable, const char *what)
{
    if (g_scoreOriginals.contains(vtable))
        return;
    REL::Relocation<std::uintptr_t> table{vtable};
    const auto original = table.write_vfunc(kCalculateScoreSlot, ScoreHook);
    g_scoreOriginals[vtable] = reinterpret_cast<ScoreFn>(original);
    logger::info("watching the AI's score of {} (vtable {:X})", what, vtable);
}

void WatchScoreOf(RE::CombatInventoryItem *entry)
{
    if (!entry)
        return;
    const auto vtable = *reinterpret_cast<const std::uintptr_t *>(entry);
    if (g_scoreOriginals.contains(vtable))
        return;
    // A class the load-time table did not name: say so, with the entry's
    // last score as a check that the slot is the scoring one.
    logger::info("an AI entry class not in the table: entries like {} (last score {:.2f})",
                 entry->item && entry->item->GetName() ? entry->item->GetName() : "?", entry->itemScore);
    WatchScoresIn(vtable, "entries of an unlisted class");
}

void ProbeCombatInventory(RE::Actor *actor)
{
    const ft::ActorId id = actor->GetFormID();
    auto *controller = actor->GetActorRuntimeData().combatController;
    if (!controller || !controller->inventory)
    {
        g_probedFights.erase(id);
        return;
    }
    if (!g_probedFights.insert(id).second)
        return;
    g_zeroedOnce.clear();
    std::unordered_set<const RE::TESForm *> listed;
    for (int slot = 0; slot < 7; ++slot)
    {
        std::string names;
        for (const auto &entry : controller->inventory->inventoryItems[slot])
        {
            const auto *form = entry ? entry->item : nullptr;
            listed.insert(form);
            names += (names.empty() ? "" : ", ") + std::string(form && form->GetName() ? form->GetName() : "?") +
                     HandTag(entry ? SlotHand(entry->itemSlot.equipSlot) : Hand::None);
            WatchScoreOf(entry.get());
        }
        logger::info("{} combat inventory [{}]: {}", Describe(actor), slot, names.empty() ? "-" : names);
    }
    // Which of her spells the AI did not list, and her magicka at the
    // moment, since a cost above the pool is the first guess at the filter
    // that kept a pinned Chain Lightning out (03:25).
    std::string missing;
    const auto consider = [&](RE::SpellItem *spell) {
        if (spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell && !listed.contains(spell))
            missing += (missing.empty() ? "" : ", ") + std::string(spell->GetName() ? spell->GetName() : "?") + " (" +
                       std::to_string(static_cast<int>(spell->CalculateMagickaCost(actor))) + ")";
    };
    if (auto *npc = actor->GetActorBase())
    {
        if (auto *list = npc->GetSpellList())
            for (std::uint32_t i = 0; i < list->numSpells; ++i)
                consider(list->spells[i]);
    }
    for (auto *spell : actor->GetActorRuntimeData().addedSpells)
        consider(spell);
    auto *owner = actor->AsActorValueOwner();
    logger::info("{} combat inventory left out: {} -- magicka {:.0f}/{:.0f}", Describe(actor),
                 missing.empty() ? "nothing" : missing, owner ? owner->GetActorValue(RE::ActorValue::kMagicka) : 0.0f,
                 owner ? owner->GetPermanentActorValue(RE::ActorValue::kMagicka) : 0.0f);
}

// Followers whose view is to be republished on the next pacing beat, whether
// or not the clock is running. A spell leaves a hand through the Papyrus
// native, which the script VM runs a frame or so after the request, so
// the view republished in the request still showed the spell in hand and a
// second click was needed to see it gone (04:15). Game thread only.
std::unordered_set<ft::ActorId> g_republish;

} // namespace

// Mark the scanned items and spells that are pinned, and those the AI is
// kept from, for the panel's cells; and drop any pin for something the
// scans did not find: sold, dropped, the last arrow shot. The watchdog
// catches that case on its own, but there is no reason to leave a dead pin
// for it to find.
void MarkPins(RE::Actor *actor, std::vector<InventoryItem> &items, std::vector<MagicEntry> &magic)
{
    std::scoped_lock lock(g_pinMutex);
    const auto it = g_pins.find(actor->GetFormID());
    if (it == g_pins.end() || it->second.empty())
        return;
    auto &pins = it->second;
    std::vector<Pin> asPlanned;
    for (const auto &[form, hands] : pins)
        asPlanned.push_back(PlannedPin(actor, form, hands));

    // The tooltip's reason: the pins in the way, one per line, "Firebolt
    // is pinned". Which hand or slot is plain from the table itself.
    const auto why = [&](const std::vector<Pin> &shadowing) {
        std::string lines;
        for (const Pin &pin : shadowing)
        {
            const auto *holder = RE::TESForm::LookupByID(pin.form);
            const char *name = holder && holder->GetName() ? holder->GetName() : "Something";
            lines += (lines.empty() ? "" : "\n") + std::string(name) + " is pinned";
        }
        return lines;
    };

    std::unordered_set<std::uint32_t> present;
    const auto mark = [&](std::uint32_t form, bool &left, bool &right, bool &aside, std::string &asideBy, bool *whole) {
        present.insert(form);
        if (const auto pin = pins.find(form); pin != pins.end())
        {
            left = Overlap(pin->second, Hand::Left);
            right = Overlap(pin->second, Hand::Right);
            if (whole)
                *whole = pin->second == Hand::None;
            return;
        }
        auto *thing = RE::TESForm::LookupByID(form);
        if (!thing)
            return;
        const Holdable described = DescribeHoldable(actor, thing);
        aside = SetAside(asPlanned, described);
        if (aside)
            asideBy = why(Shadowing(asPlanned, described));
    };
    for (auto &item : items)
        mark(item.form, item.pinnedLeft, item.pinnedRight, item.setAside, item.asideBy, &item.pinned);
    for (auto &entry : magic)
        mark(entry.form, entry.pinnedLeft, entry.pinnedRight, entry.setAside, entry.asideBy, nullptr);
    std::erase_if(pins, [&](const auto &pin) { return !present.contains(pin.first); });
}

bool EquipSpellIn(RE::Actor *actor, RE::SpellItem *spell, Hand hand)
{
    auto *manager = RE::ActorEquipManager::GetSingleton();
    if (!manager || !actor || !spell)
        return false;
    const auto &data = actor->GetActorRuntimeData();
    const bool left = data.selectedSpells[RE::Actor::SlotTypes::kLeftHand] == spell;
    const bool right = data.selectedSpells[RE::Actor::SlotTypes::kRightHand] == spell;
    const bool already = hand == Hand::Left ? left : hand == Hand::Right ? right : (left || right);
    if (already)
        return false;
    manager->EquipSpell(actor, spell, hand == Hand::None ? nullptr : HandSlot(hand));
    return true;
}

void WatchCombatScores()
{
    // Every entry class that can hold a hand: the weapon kinds, and the
    // spell entry for each kind of caster the AI has. Potions, scrolls and
    // shouts take no hand and are never kept from it.
    struct Named
    {
        const std::array<REL::VariantID, 1> *id;
        const char *what;
    };
    const Named classes[] = {
        {&RE::VTABLE_CombatInventoryItemMelee, "melee weapons"},
        {&RE::VTABLE_CombatInventoryItemRanged, "bows and crossbows"},
        {&RE::VTABLE_CombatInventoryItemShield, "shields"},
        {&RE::VTABLE_CombatInventoryItemOneHandedBlock, "one-handed blocking"},
        {&RE::VTABLE_CombatInventoryItemTorch, "torches"},
        {&RE::VTABLE_CombatInventoryItemStaff, "staves"},
        {&RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterOffensive_, "attack spells"},
        {&RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterRestore_, "healing spells"},
        {&RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterWard_, "wards"},
        {&RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterArmor_, "armour spells"},
        {&RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterSummon_, "summons"},
        {&RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterBoundItem_, "bound weapons"},
        {&RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterCloak_, "cloaks"},
        {&RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterInvisibility_, "invisibility"},
        {&RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterLight_, "light spells"},
        {&RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterDisarm_, "disarm spells"},
        {&RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterParalyze_, "paralysis"},
        {&RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterReanimate_, "reanimation"},
        {&RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterStagger_, "stagger spells"},
        {&RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterTargetEffect_,
         "target-effect spells"},
        {&RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterScript_, "scripted spells"},
    };
    for (const Named &named : classes)
    {
        const REL::Relocation<std::uintptr_t> table{(*named.id)[0]};
        WatchScoresIn(table.address(), named.what);
    }
}

void RepublishOwed()
{
    // Views owed after a spell unequip: the
    // clock is frozen while the panel is open, and this is what the panel
    // is waiting on.
    for (const ft::ActorId id : g_republish)
    {
        if (auto *actor = RE::TESForm::LookupByID<RE::Actor>(id))
        {
            logger::info("{} time running again -- {}", Describe(actor), CasterState(actor));
            PublishFollower(actor);
        }
    }
    g_republish.clear();
}

void KeepPins(const std::vector<RE::Actor *> &followers)
{
    EnforcePins(followers);
    for (auto *follower : followers)
        ProbeCombatInventory(follower);
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
    task->AddTask([id, form, request, hand]() mutable {
        auto *actor = RE::TESForm::LookupByID<RE::Actor>(id);
        auto *thing = RE::TESForm::LookupByID(form);
        if (!actor || !thing)
            return;

        const Holdable described = DescribeHoldable(actor, thing);
        Hand hands = HandsFor(described.grip, hand);
        if (request == WearRequest::Pin && !Pinnable(described))
        {
            // The panel does not offer this, but a pin is a promise, and it
            // is kept here too: the AI would not choose it, so equip only.
            logger::info("{} {} cannot be pinned (the AI would not choose it); equipping instead", Describe(actor),
                         thing->GetName() ? thing->GetName() : "?");
            request = WearRequest::Equip;
        }
        // One weapon cannot be in both hands. Asked to move her only copy to
        // the other hand, take it out of the first; otherwise the engine's
        // equip, finding none free, conjures a second (02:05, the doubled
        // dagger). Two in the bag may go one per hand.
        bool moving = false;
        if (request == WearRequest::Pin && thing->Is(RE::FormType::Weapon) &&
            (hands == Hand::Left || hands == Hand::Right))
        {
            const Hand other = hands == Hand::Left ? Hand::Right : Hand::Left;
            if (EquippedIn(actor, thing, other))
            {
                auto *object = thing->As<RE::TESBoundObject>();
                auto inventory = actor->GetInventory([object](RE::TESBoundObject &c) { return &c == object; });
                const auto found = inventory.find(object);
                moving = (found != inventory.end() ? found->second.first : 0) < 2;
            }
        }

        {
            std::scoped_lock lock(g_pinMutex);
            auto &pins = g_pins[id];
            const auto pin = pins.find(form);
            const bool eitherHand = described.grip == Grip::Either && pin != pins.end() && hand != Hand::None;
            if (request == WearRequest::Pin || request == WearRequest::Equip)
            {
                ReleaseConflictingPins(actor, pins, thing, hands);
                // An either-hand thing already pinned in the other hand is
                // pinned in both now, a spell once in each; unless this is
                // her one weapon changing hands.
                if (request == WearRequest::Pin)
                    pins[form] = eitherHand && !moving ? pin->second | hands : hands;
            }
            else if (pin != pins.end())
            {
                // One cell of a two-handed pin lets that hand go and keeps
                // the other; anything else is the whole pin.
                if (eitherHand && pin->second == Hand::Both)
                {
                    pin->second = Without(pin->second, hand);
                    hands = hand;
                }
                else
                {
                    hands = pin->second;
                    pins.erase(pin);
                }
            }
        }

        const char *name = thing->GetName() ? thing->GetName() : "?";
        switch (request)
        {
        case WearRequest::Equip:
            logger::info("{} told to ready {} (not pinned)", Describe(actor), name);
            EquipPinned(actor, thing, hands, true);
            break;
        case WearRequest::Pin:
            logger::info("{} told to ready {} (pinned)", Describe(actor), name);
            // Off for now, to see what her own style does with a left-hand
            // weapon; the copy stays available for the combat-style work.
            if constexpr (kDualWieldOnLeftPin)
            {
                if (thing->Is(RE::FormType::Weapon) && hands == Hand::Left)
                    AllowDualWield(actor);
            }
            if (moving)
                UnequipForm(actor, thing, hands == Hand::Left ? Hand::Right : Hand::Left, true);
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
                logger::info("{} spell {} asked {} -- record slot {:06X} -- now left {} right {} -- {}",
                             Describe(actor), name, static_cast<int>(hands), slot ? slot->GetFormID() : 0,
                             data.selectedSpells[RE::Actor::SlotTypes::kLeftHand] == spell,
                             data.selectedSpells[RE::Actor::SlotTypes::kRightHand] == spell, CasterState(actor));
                // Look again once time runs, to see what the engine finishes.
                g_republish.insert(id);
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
            if (thing->Is(RE::FormType::Spell))
                g_republish.insert(id);
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

        PublishFollower(actor);
    });
}

} // namespace ft::game
