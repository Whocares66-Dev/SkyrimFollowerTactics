// Pins: what stays in a hand, and how the promise is kept. The rules are
// core/Loadout.cpp; this is where they meet the engine. Everything here runs
// on the game thread -- the tick, or a task the panel queued.

#include "game/Pins.h"

#include "game/Sensors.h"

#include "game/Packages.h"

#include "game/Tactics.h"
#include "game/Util.h"

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Detours needs the Windows API declared first, and asks for it by name.
#include <Windows.h>

#include <detours/detours.h>

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
std::unordered_map<ft::ActorId, std::vector<Pin>> g_pins;
// What the panel has banned on each follower. Same lock, same thread.
std::unordered_map<ft::ActorId, Bans> g_bans;

bool BannedHere(ft::ActorId id, std::uint32_t form)
{
    const auto it = g_bans.find(id);
    return it != g_bans.end() && IsBanned(it->second, form);
}

// Above zero while an equip is OURS. The engine's equips and ours reach the
// same hook (RefuseEquipsAgainstPins), and only the engine's are ever
// refused. Per thread: an equip is synchronous, and the counter must not
// leak between the game thread and a task.
thread_local int g_ownEquipDepth = 0;

struct OwnEquip
{
    OwnEquip() noexcept
    {
        ++g_ownEquipDepth;
    }
    ~OwnEquip()
    {
        --g_ownEquipDepth;
    }
    OwnEquip(const OwnEquip &) = delete;
    OwnEquip &operator=(const OwnEquip &) = delete;
};

// Refusals already logged, one per follower and thing: the engine asks
// again every frame or so, and one line says it. Cleared when her pins
// change, so a fresh conflict is logged afresh.
std::unordered_set<std::uint64_t> g_refusedLogged;

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

} // namespace

// The Voice equip slot record, Skyrim.esm (beside the hand slots below).
constexpr std::uint32_t kVoiceSlotID = 0x00025BEE;

namespace
{
// How many of an item she carries, for the one-copy rule. A spell is not
// an item and needs no count.
int CarriedCount(RE::Actor *actor, RE::TESBoundObject *object)
{
    if (!actor || !object)
        return 0;
    auto inventory = actor->GetInventory([object](RE::TESBoundObject &c) { return &c == object; });
    const auto found = inventory.find(object);
    return found == inventory.end() ? 0 : found->second.first;
}
} // namespace

// The rules themselves are in core/Loadout.cpp, where they are tested.
Holdable DescribeHoldable(RE::Actor *actor, RE::TESForm *form)
{
    Holdable thing;
    thing.form = form->GetFormID();
    if (auto *weapon = form->As<RE::TESObjectWEAP>())
    {
        const bool bothHands =
            weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() || weapon->IsCrossbow();
        thing.kind = Kind::Weapon;
        thing.grip = bothHands ? Grip::Both : Grip::Either;
        thing.count = CarriedCount(actor, weapon);
    }
    else if (auto *spell = form->As<RE::SpellItem>())
    {
        const auto type = spell->GetSpellType();
        if (type == RE::MagicSystem::SpellType::kPower || type == RE::MagicSystem::SpellType::kLesserPower ||
            IsLeasedPower(spell->GetFormID()))
        {
            thing.kind = Kind::Voice; // readied in the voice slot, no hand
            return thing;
        }
        if (type != RE::MagicSystem::SpellType::kSpell)
            return thing; // an ability, a disease: nothing to hold
        thing.kind = Kind::Spell;
        // The slot records, by FormID from Skyrim.esm: RightHand 013F42,
        // LeftHand 013F43, EitherHand 013F44, BothHands 013F45. The default
        // object table did not answer for them on this game.
        const auto *slot = spell->GetEquipSlot();
        const std::uint32_t slotId = slot ? slot->GetFormID() : 0;
        thing.grip = spell->IsTwoHanded()   ? Grip::Both
                     : slotId == 0x00013F43 ? Grip::LeftOnly
                     : slotId == 0x00013F42 ? Grip::RightOnly
                                            : Grip::Either;
        // Above the follower's skill in its school: the combat AI will not
        // choose it. An effect of no school (a power's, an ability's) has
        // no skill to ask about -- its skill is kNone, and asking for that
        // actor value must not happen: the engine's own getter shrugs it
        // off, but ActorValueExtension's hook of it indexes a table with
        // the number and crashes (Nordic Souls, 2026-09-08, on Serana).
        const auto *costliest = spell->GetCostliestEffectItem();
        const auto *effect = costliest ? costliest->baseEffect : nullptr;
        auto *owner = actor->AsActorValueOwner();
        const auto school = effect ? effect->GetMagickSkill() : RE::ActorValue::kNone;
        if (effect && owner && school != RE::ActorValue::kNone)
            thing.unusable = effect->GetMinimumSkillLevel() > owner->GetActorValue(school);
    }
    else if (auto *armor = form->As<RE::TESObjectARMO>())
    {
        thing.slots = static_cast<std::uint32_t>(armor->GetSlotMask().underlying());
        thing.grip = ArmorGrip(armor);
        // A shield, or a mod's hand-held piece, is a weapon to the rules as
        // it is to the panel: chosen with the sword, and it takes a hand.
        thing.kind = thing.grip == Grip::None ? Kind::Armor : Kind::Weapon;
    }
    else if (form->Is(RE::FormType::Light))
    {
        thing.kind = Kind::Weapon;
        thing.grip = Grip::LeftOnly;
    }
    else if (form->Is(RE::FormType::Ammo))
    {
        thing.kind = Kind::Ammo;
    }
    else if (form->Is(RE::FormType::Shout))
    {
        thing.kind = Kind::Voice;
    }
    return thing;
}

// Is this form readied in the voice slot: a power or a shout the actor has
// selected? Voice things have no hand; this is their "equipped".
bool InVoice(RE::Actor *actor, RE::TESForm *form)
{
    return actor && actor->GetActorRuntimeData().selectedPower == form;
}

std::vector<Pin> PinsOf(ft::ActorId id)
{
    std::scoped_lock lock(g_pinMutex);
    const auto it = g_pins.find(id);
    return it == g_pins.end() ? std::vector<Pin>{} : it->second;
}

Bans BansOf(ft::ActorId id)
{
    std::scoped_lock lock(g_pinMutex);
    const auto it = g_bans.find(id);
    return it == g_bans.end() ? Bans{} : it->second;
}

namespace
{

// Is the form in those hands right now?
bool EquippedIn(RE::Actor *actor, RE::TESForm *form, Hand hands)
{
    if (DescribeHoldable(actor, form).IsVoice())
        return InVoice(actor, form);
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
// time running, keeps the queue.
//
// WITHOUT the engine's prevent-removal flag, since 2026-09-04. The flag is
// worn-item state that lives in the save, so it outlived the mod: with the
// DLL removed, the engine's equip-best swap had its unequip of a pinned
// dagger refused by the flag while its equip of the new sword went ahead,
// and the follower stood with both marked equipped in one hand. The pin is
// kept by the equip detour, the score hook and the watchdog, all of which
// exist only while the DLL does -- so now the pin does too, and nothing of
// ours is left in a save.
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
    const auto *voice = data.selectedPower;
    return std::string("selected L=") + name(data.selectedSpells[RE::Actor::SlotTypes::kLeftHand]) +
           " R=" + name(data.selectedSpells[RE::Actor::SlotTypes::kRightHand]) +
           " voice=" + (voice && voice->GetName() ? voice->GetName() : "-") +
           " -- caster L=" + name(left ? left->currentSpell : nullptr) +
           " R=" + name(right ? right->currentSpell : nullptr);
}

void EquipPinned(RE::Actor *actor, RE::TESForm *form, Hand hands, bool now)
{
    auto *manager = RE::ActorEquipManager::GetSingleton();
    if (!manager)
        return;
    // The voice slot: a shout by the manager's own call, a power by the
    // spell equip with the Voice slot record (025BEE). Both a no-op when
    // already readied, as the hand equip is.
    if (auto *shout = form->As<RE::TESShout>())
    {
        if (!InVoice(actor, shout))
            manager->EquipShout(actor, shout);
        return;
    }
    if (auto *spell = form->As<RE::SpellItem>(); spell && DescribeHoldable(actor, spell).IsVoice())
    {
        if (!InVoice(actor, spell))
            manager->EquipSpell(actor, spell, RE::TESForm::LookupByID<RE::BGSEquipSlot>(kVoiceSlotID));
        return;
    }
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
    const OwnEquip ours;
    // With the engine's equip sound, at the actor, as when a follower is
    // handed armour. The watchdog's putting-back sounds too: it only acts
    // when the thing is actually off, so each sound marks a real event.
    manager->EquipObject(actor, object, nullptr, 1, slot, !now, false, true, false);
}

// Take a form off.
//
// Items go through the equip manager WITHOUT the prevent-equip flag: the
// Creation Kit wiki notes that flag does nothing for weapons on an NPC and
// works only too well for ammunition, leaving an archer holding a bow she
// cannot use. A spell has no unequip in CommonLibSSE or in SKSE; the
// engine's is the Papyrus native Actor.UnequipSpell(spell, source), 0 for
// the left hand and 1 for the right, so it is dispatched to the script VM,
// which runs it on the game thread a frame later.
// Is the thing on anywhere it could be, and off from everywhere it is. An
// either-hand thing is asked about, and taken from, each hand in turn: the
// item code reads "both hands" as a two-hander's, which lives in the right,
// and a ban on a dagger in the left hand found nothing to take off (16:27,
// Marcurio's iron dagger).
bool OnAnywhere(RE::Actor *actor, RE::TESForm *form, const Holdable &described);
void TakeOffEverywhere(RE::Actor *actor, RE::TESForm *form, const Holdable &described, bool now);

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
    // The voice: Papyrus's UnequipShout for a shout, UnequipSpell with the
    // voice source (2) for a power. Neither has a native in CommonLibSSE,
    // as a hand spell's unequip has not.
    if (DescribeHoldable(actor, form).IsVoice())
    {
        if (!InVoice(actor, form))
            return;
        auto *vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        auto *policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
        if (!policy)
            return;
        const auto handle = policy->GetHandleForObject(actor->GetFormType(), actor);
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;
        if (auto *shout = form->As<RE::TESShout>())
            vm->DispatchMethodCall2(handle, "Actor", "UnequipShout", RE::MakeFunctionArguments(std::move(shout)),
                                    result);
        else if (auto *power = form->As<RE::SpellItem>())
            vm->DispatchMethodCall2(handle, "Actor", "UnequipSpell",
                                    RE::MakeFunctionArguments(std::move(power), static_cast<std::int32_t>(2)), result);
        return;
    }
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

bool OnAnywhere(RE::Actor *actor, RE::TESForm *form, const Holdable &described)
{
    if (described.IsVoice())
        return InVoice(actor, form);
    if (described.grip == Grip::Either)
        return EquippedIn(actor, form, Hand::Left) || EquippedIn(actor, form, Hand::Right);
    if (described.grip == Grip::None)
    {
        auto *object = form->As<RE::TESBoundObject>();
        return object && Worn(actor, object, Hand::None);
    }
    return EquippedIn(actor, form, Reach(described.grip));
}

void TakeOffEverywhere(RE::Actor *actor, RE::TESForm *form, const Holdable &described, bool now)
{
    if (described.grip == Grip::Either)
    {
        UnequipForm(actor, form, Hand::Left, now);
        UnequipForm(actor, form, Hand::Right, now);
        return;
    }
    UnequipForm(actor, form, described.grip == Grip::None ? Hand::None : Reach(described.grip), now);
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

// Before something new goes on, whatever it displaces is unpinned, so the
// book and the body agree: the engine's own displacement would leave the
// old pin in the book, and the watchdog would put it straight back over the
// new thing. Taking it off is the engine's, in the equip that follows.
std::vector<Displaced> ReleaseConflictingPins(RE::Actor *actor, std::vector<Pin> &pins, const Holdable &incoming,
                                              Hand hands, bool dualWield)
{
    // Only the book changes here. What gave way is NOT taken off: the
    // engine's equip displaces it -- a weapon or spell from the hand it
    // takes, armour from shared body slots, arrows from the quiver, a power
    // or shout from the voice -- so the explicit unequip that used to follow
    // did nothing for items and spells and undid the new equip for the
    // voice (its spell unequip is a deferred Papyrus call, which landed a
    // frame after the new power was in and emptied the slot, 2026-09-05).
    // The unequip was needed while pinned items carried the engine's
    // prevent-removal flag, which refused the engine's own swap; the flag
    // went on 2026-09-04, and this went with it. The one unequip that stays
    // is the move of a follower's only weapon to the other hand, in Wear.
    std::vector<Displaced> displaced = MakeRoom(pins, incoming, hands, dualWield);
    for (const Displaced &gone : displaced)
    {
        auto *held = RE::TESForm::LookupByID(gone.form);
        logger::info("{} unpinning {}{} to make room", Describe(actor), held && held->GetName() ? held->GetName() : "?",
                     HandTag(gone.hands));
    }
    return displaced;
}

// The watchdog: put back any pinned form the game has taken off, and forget
// pins for items no longer carried.
//
// Its own pass, not a rider on the inventory scan, and gated on the pin map:
// with nothing pinned it costs a lock and a look at an empty map. With pins
// it asks the engine for each pinned item's entry alone -- a walk of pointer
// compares, no names, no strings -- so a follower with two pins costs two
// lookups a tick, not a sweep of the bag.
//
// When a pin goes back is the core's PutBackNow, tested: at once, except a
// hand pin while one of OUR casts is in progress -- the UseMagic package
// has the hand, and the dagger goes back once the spell has left it. The
// first version stood every hand pin down for the whole fight, against the
// flicker of the combat AI wanting a pinned hand for its own spell; the
// score hook now keeps the AI's choice off a pinned hand, and the
// stand-down left Marcurio dagger-less from his first Lightning Bolt to the
// end of the fight (12:37).
// For the log: what each hand holds right now, spell or item.
std::string HandsState(RE::Actor *actor)
{
    const auto name = [](const RE::TESForm *form) { return form && form->GetName() ? form->GetName() : "-"; };
    return CasterState(actor) + " -- held L=" + name(actor->GetEquippedObject(true)) +
           " R=" + name(actor->GetEquippedObject(false));
}

// A follower and a form, as one key.
std::uint64_t ReadyKey(const RE::Actor *actor, const RE::TESForm *form)
{
    return (static_cast<std::uint64_t>(actor->GetFormID()) << 32) | form->GetFormID();
}

// --- pins over a fight -------------------------------------------------------
//
// What the book held when a fight began is put back when it ends. The rules
// pin for the fight -- the bow at range, Flames over the travelling dagger,
// None to let the AI choose -- and every one of those displaces or drops a
// pin the player made in the panel. The player's pins are what the follower
// travels in, and a fight should not rewrite them: the outfit goes back on,
// the dagger goes back in hand, the rule's bow is let go. A pin the player
// makes in the panel DURING the fight is applied to the remembered book
// too, so it is what comes back. Keyed by follower; the transitions are
// read on the watchdog's tick, which already asks IsInCombat every half
// second.
std::unordered_map<ft::ActorId, std::vector<Pin>> g_pinsBeforeFight;
std::unordered_set<ft::ActorId> g_fighting;

// The fight is over: what it pinned is let go, what it displaced is pinned
// again, and the watchdog, running next in the same pass and now out of
// combat, puts the restored pins back on. Which is which is the core's
// SettleAfterFight, tested: a fight's pin is let go IN PLACE -- the bow
// stays in hand, the cuirass stays on, the AI's to change as it likes --
// unless something from before the fight is coming back to that hand or
// slot, in which case it is taken off to make way. Taking everything off
// left Jenassa stripped after a fight that began with an empty book
// (21:02). Under g_pinMutex.
void RestorePinsAfterFight(RE::Actor *actor, std::vector<Pin> &pins, const std::vector<Pin> &before)
{
    const AfterFight settle = SettleAfterFight(pins, before);
    for (const Released &gone : settle.released)
    {
        auto *thing = RE::TESForm::LookupByID(gone.form);
        if (!thing)
            continue;
        const char *name = thing->GetName() ? thing->GetName() : "?";
        if (gone.takeOff)
        {
            logger::info("{} fight over -- {}{} pinned during it comes off; what was there before comes back",
                         Describe(actor), name, HandTag(gone.hands));
            UnequipForm(actor, thing, gone.hands, true);
            continue;
        }
        // Forgetting the pin is the whole of it: nothing on the item marks
        // it pinned, so there is nothing to lift.
        logger::info("{} fight over -- {}{} pinned during it stays on, unpinned", Describe(actor), name,
                     HandTag(gone.hands));
    }
    for (const Pin &pin : settle.restored)
    {
        const auto *thing = RE::TESForm::LookupByID(pin.thing.form);
        logger::info("{} fight over -- {}{} pinned again, as before it", Describe(actor),
                     thing && thing->GetName() ? thing->GetName() : "?", HandTag(pin.hands));
    }
    pins = before;
    if (settle.released.empty() && settle.restored.empty())
        return;
    // A fresh start for the refusal log: the book is what it was.
    g_refusedLogged.clear();
    actor->Update3DModel();
}

// Note a follower entering or leaving combat. Under g_pinMutex.
void NoteFight(RE::Actor *actor, std::vector<Pin> &pins, bool fighting)
{
    const ft::ActorId id = actor->GetFormID();
    const bool was = g_fighting.contains(id);
    if (fighting && !was)
    {
        g_fighting.insert(id);
        g_pinsBeforeFight[id] = pins;
        if (!pins.empty())
            logger::info("{} fight begins -- {} pin(s) remembered for after it", Describe(actor), pins.size());
        return;
    }
    if (!fighting && was)
    {
        g_fighting.erase(id);
        if (auto saved = g_pinsBeforeFight.extract(id); !saved.empty())
            RestorePinsAfterFight(actor, pins, saved.mapped());
    }
}

void EnforcePins(const std::vector<RE::Actor *> &followers)
{
    std::scoped_lock lock(g_pinMutex);

    for (auto *actor : followers)
    {
        // Every follower, pins or none: a fight that begins with an empty
        // book and ends with a rule's pin in it still has to be put right.
        auto &pins = g_pins[actor->GetFormID()];
        const bool fighting = actor->IsInCombat();
        NoteFight(actor, pins, fighting);
        if (pins.empty())
            continue;

        for (auto pin = pins.begin(); pin != pins.end();)
        {
            auto *form = RE::TESForm::LookupByID(pin->thing.form);
            if (!form)
            {
                pin = pins.erase(pin);
                continue;
            }
            const Hand hands = pin->hands;

            // Spells first: a SpellItem is a bound object too, and the
            // inventory branch dropped every spell pin as "no longer
            // carried" (00:26, Close Wounds).
            // Out of combat the engine's equip-best put a sword over pinned
            // Flames every update, and this put it back every tick -- the
            // draw loop of 17:31. The equip detour refuses that sword now,
            // for items as for spells; if this line repeats, the detour has
            // missed a path, and the hand state beside it says which.
            const bool casting = IsMidCast(actor);
            if (pin->thing.IsVoice())
            {
                // A shout is no bound object and a power is no hand spell:
                // readied or not is the voice slot, and back it goes.
                if (PutBackNow(*pin, InVoice(actor, form), fighting, casting))
                {
                    logger::info("{} put away pinned {} -- readying it in the voice again", Describe(actor),
                                 form->GetName() ? form->GetName() : "?");
                    EquipPinned(actor, form, hands, false);
                }
            }
            else if (form->Is(RE::FormType::Spell))
            {
                if (PutBackNow(*pin, EquippedIn(actor, form, hands), fighting, casting))
                {
                    logger::info("{} put away pinned {} -- readying it again -- {}", Describe(actor),
                                 form->GetName() ? form->GetName() : "?", HandsState(actor));
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
                if (PutBackNow(*pin, on, fighting, casting))
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

    // A banned thing found on comes off -- unless a pin holds it (a rule's
    // instruction, the player's own, wins for as long as it lasts) or one
    // of our casts has the hand. The score hook and the equip detour keep
    // this from happening; this is what answers it when it has.
    for (auto *actor : followers)
    {
        const auto it = g_bans.find(actor->GetFormID());
        if (it == g_bans.end() || it->second.empty())
            continue;
        const auto pinsIt = g_pins.find(actor->GetFormID());
        const std::vector<Pin> *pins = pinsIt == g_pins.end() ? nullptr : &pinsIt->second;
        if (IsMidCast(actor))
            continue;
        for (const std::uint32_t form : it->second)
        {
            if (pins && FindPin(*pins, form))
                continue;
            auto *thing = RE::TESForm::LookupByID(form);
            if (!thing)
                continue;
            const Holdable described = DescribeHoldable(actor, thing);
            if (!OnAnywhere(actor, thing, described))
                continue;
            logger::info("{} has banned {} on -- taking it off", Describe(actor),
                         thing->GetName() ? thing->GetName() : "?");
            TakeOffEverywhere(actor, thing, described, false);
        }
    }
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

// Kept from the AI: banned, or pinned against. A ban is looked up by form
// alone, whatever hand the entry is for.
bool ShadowedEntry(RE::CombatInventoryItem *entry, RE::Actor *actor, const char *&why)
{
    if (!entry || !entry->item || !actor)
        return false;
    {
        std::scoped_lock lock(g_pinMutex);
        if (BannedHere(actor->GetFormID(), entry->item->GetFormID()))
        {
            why = "banned";
            return true;
        }
    }
    const Holdable thing = DescribeHoldable(actor, entry->item);
    const Hand slot = SlotHand(entry->itemSlot.equipSlot);
    // One copy of a weapon, already in the other hand: the entry for this
    // hand cannot be honoured, and the engine, asked anyway, shows the one
    // object in both hands. A sword never reaches this -- the melee AI
    // fills the left hand only under the dual-wield rules -- but a staff
    // is a weapon the AI handles as magic, listed and equipped per hand as
    // a spell is, with no count behind it (Jenassa's one Staff of Flames
    // in both hands, 18:56).
    if (thing.kind == Kind::Weapon && thing.count < 2 && (slot == Hand::Left || slot == Hand::Right) &&
        actor->GetEquippedObject(slot != Hand::Left) == entry->item)
    {
        why = "her only one, in the other hand";
        return true;
    }
    const std::vector<Pin> pins = PinsOf(actor->GetFormID());
    if (pins.empty())
        return false;
    why = "pinned against";
    return KeptFromAI(pins, thing, slot);
}

float ScoreHook(RE::CombatInventoryItem *self, RE::CombatController *controller)
{
    const auto vtable = *reinterpret_cast<const std::uintptr_t *>(self);
    const auto original = g_scoreOriginals.find(vtable);
    const float score = original != g_scoreOriginals.end() ? original->second(self, controller) : 0.0f;
    const RE::NiPointer<RE::Actor> actor = AttackerOf(controller);
    const char *why = "";
    if (!ShadowedEntry(self, actor.get(), why))
        return score;
    if (g_zeroedOnce.insert(self).second)
    {
        logger::info("{} AI asked the score of {}{}: {:.2f}, answered 0 ({})", Describe(actor.get()),
                     self->item->GetName() ? self->item->GetName() : "?", HandTag(SlotHand(self->itemSlot.equipSlot)),
                     score, why);
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
    if (const auto bans = g_bans.find(actor->GetFormID()); bans != g_bans.end())
    {
        for (auto &item : items)
            item.banned = IsBanned(bans->second, item.form);
        for (auto &entry : magic)
            entry.banned = IsBanned(bans->second, entry.form);
    }
    const auto it = g_pins.find(actor->GetFormID());
    if (it == g_pins.end() || it->second.empty())
        return;
    auto &pins = it->second;
    const std::vector<Pin> asPlanned = pins;
    const bool dualWield = DualWieldAllowed(actor);

    // The tooltip's reason: the pins in the way, one per line, "Firebolt
    // is pinned". Which hand or slot is plain from the table itself.
    const auto why = [&](const std::vector<Pin> &shadowing) {
        std::string lines;
        for (const Pin &pin : shadowing)
        {
            const auto *holder = RE::TESForm::LookupByID(pin.thing.form);
            const char *name = holder && holder->GetName() ? holder->GetName() : "Something";
            lines += (lines.empty() ? "" : "\n") + std::string(name) + " is pinned";
        }
        return lines;
    };

    // The detail page's Equipped row is built by the scan, before the pins
    // are known; the pin glyph goes on it here.
    const auto pinGlyph = [](std::vector<SheetSection> &detail) {
        for (auto &section : detail)
            for (auto &row : section.rows)
                if (row.label == "Equipped" && row.icon == kGlyphTick)
                    row.icon2 = kGlyphPin;
    };

    std::unordered_set<std::uint32_t> present;
    const auto mark = [&](std::uint32_t form, bool &left, bool &right, bool &aside, std::string &asideBy, bool &whole,
                          std::vector<SheetSection> &detail) {
        present.insert(form);
        if (const Pin *pin = FindPin(pins, form))
        {
            left = Overlap(pin->hands, Hand::Left);
            right = Overlap(pin->hands, Hand::Right);
            whole = pin->hands == Hand::None;
            pinGlyph(detail);
            return;
        }
        auto *thing = RE::TESForm::LookupByID(form);
        if (!thing)
            return;
        const Holdable described = DescribeHoldable(actor, thing);
        aside = SetAside(asPlanned, described);
        if (aside)
            asideBy = why(Shadowing(asPlanned, described));
        // A one-hander beside a one-hander pinned in either hand, where the
        // style forbids two: greyed, and the reason on the name. Its cells
        // still take a click -- into the other hand, and the pinned one
        // comes off (Wear).
        if (!aside && !dualWield)
        {
            for (const Pin &pin : asPlanned)
            {
                if ((pin.hands == Hand::Left || pin.hands == Hand::Right) && pin.thing.form != form &&
                    WouldDualWield(described, &pin.thing))
                {
                    aside = true;
                    asideBy = "Cannot dual wield";
                    break;
                }
            }
        }
    };
    for (auto &item : items)
        mark(item.form, item.pinnedLeft, item.pinnedRight, item.setAside, item.asideBy, item.pinned, item.detail);
    for (auto &entry : magic)
        mark(entry.form, entry.pinnedLeft, entry.pinnedRight, entry.setAside, entry.asideBy, entry.pinned,
             entry.detail);
    std::erase_if(pins, [&](const Pin &pin) { return !present.contains(pin.thing.form); });
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

namespace
{

// One request against the book, on the game thread: the panel's task and
// the rules' tick both come here.
void Wear(RE::Actor *actor, RE::TESForm *thing, WearRequest request, Hand hand, bool fromPanel)
{
    const ft::ActorId id = actor->GetFormID();
    const Holdable described = DescribeHoldable(actor, thing);
    Hand hands = HandsFor(described.grip, hand);
    if (request == WearRequest::Pin && !Pinnable(described))
    {
        // Neither the panel nor the rules offer this; a pin is a promise the
        // AI would not keep, and it is refused here too rather than
        // half-kept as an equip without a pin.
        logger::warn("{} {} cannot be pinned: above the follower's skill, the AI would not choose it", Describe(actor),
                     thing->GetName() ? thing->GetName() : "?");
        return;
    }
    // One weapon cannot be in both hands. Asked to move her only copy to
    // the other hand, take it out of the first; otherwise the engine's
    // equip, finding none free, conjures a second (02:05, the doubled
    // dagger). Two in the bag may go one per hand.
    bool moving = false;
    if ((request == WearRequest::Pin || request == WearRequest::Equip) && thing->Is(RE::FormType::Weapon) &&
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

    // A ban takes the whole thing off, whichever hands it is in.
    const Hand reach = described.grip == Grip::None ? Hand::None : Reach(described.grip);
    if (request == WearRequest::Ban || request == WearRequest::Unban)
        hands = reach;

    // Where the combat style forbids dual wielding, a one-hander into one
    // hand displaces a one-hander PINNED in the other (core/Loadout.h,
    // Conflicts): its pin goes, and since the engine's equip of one hand
    // leaves the other alone, the weapon is taken off below as well.
    const bool dualWield = DualWieldAllowed(actor);
    {
        std::scoped_lock lock(g_pinMutex);
        auto &pins = g_pins[id];
        switch (request)
        {
        case WearRequest::Pin:
            ReleaseConflictingPins(actor, pins, described, hands, dualWield);
            AddPin(pins, described, hands, moving);
            break;
        case WearRequest::Equip:
            // The AI's to change afterwards; but a pin in the way would put
            // its thing straight back, so the click lets that pin go. Her
            // only copy of a weapon changing hands takes its own pin with
            // it: a pin on the hand it is leaving would stand over an empty
            // hand (16:28, the steel dagger pinned left and held right).
            ReleaseConflictingPins(actor, pins, described, hands, dualWield);
            if (moving) [[maybe_unused]]
                const Hand left = LetGo(pins, described, hands == Hand::Left ? Hand::Right : Hand::Left);
            break;
        case WearRequest::Ban: {
            [[maybe_unused]] const Hand let = LetGo(pins, described, Hand::None);
            Ban(g_bans[id], described.form);
            break;
        }
        case WearRequest::Unban:
            Unban(g_bans[id], described.form);
            break;
        case WearRequest::Unpin:
        case WearRequest::TakeOff:
            hands = LetGo(pins, described, hands);
            break;
        }
        // The panel's word mid-fight is the new normal: the same change goes
        // into the book remembered for after the fight, so the player's pin
        // is what comes back, not the one it replaced. A rule's pin is for
        // the fight only and leaves the remembered book alone.
        if (fromPanel && g_fighting.contains(id))
        {
            auto &before = g_pinsBeforeFight[id];
            if (request == WearRequest::Pin)
            {
                [[maybe_unused]] const auto displaced = MakeRoom(before, described, hands, dualWield);
                AddPin(before, described, hands, moving);
            }
            else if (request == WearRequest::Equip)
            {
                [[maybe_unused]] const auto displaced = MakeRoom(before, described, hands, dualWield);
                if (moving) [[maybe_unused]]
                    const Hand left = LetGo(before, described, hands == Hand::Left ? Hand::Right : Hand::Left);
            }
            else if (request != WearRequest::Unban)
            {
                [[maybe_unused]] const Hand gone =
                    LetGo(before, described, request == WearRequest::Ban ? Hand::None : HandsFor(described.grip, hand));
            }
        }
        g_refusedLogged.clear();
    }

    const char *name = thing->GetName() ? thing->GetName() : "?";
    // Where the style forbids two, a one-hander into one hand takes the
    // one-hander out of the other, pinned (its pin went above) or merely
    // equipped: the engine's equip of this hand leaves the other as it
    // was, and the follower would stand with a weapon in each. The only
    // copy moving across is not this (WouldDualWield).
    if (!dualWield && (request == WearRequest::Pin || request == WearRequest::Equip) &&
        (hands == Hand::Left || hands == Hand::Right))
    {
        const Hand other = hands == Hand::Left ? Hand::Right : Hand::Left;
        if (auto *held = actor->GetEquippedObject(other == Hand::Left))
        {
            const Holdable inOther = DescribeHoldable(actor, held);
            if (WouldDualWield(described, &inOther))
            {
                logger::info("{} {} comes off the {} hand: the combat style does not dual wield", Describe(actor),
                             held->GetName() ? held->GetName() : "?", other == Hand::Left ? "left" : "right");
                UnequipForm(actor, held, other, true);
            }
        }
    }
    switch (request)
    {
    case WearRequest::Equip:
        logger::info("{} told to ready {}{} (not pinned)", Describe(actor), name, HandTag(hands));
        if (moving)
            UnequipForm(actor, thing, hands == Hand::Left ? Hand::Right : Hand::Left, true);
        EquipPinned(actor, thing, hands, true);
        if (thing->Is(RE::FormType::Spell) || thing->Is(RE::FormType::Shout))
            g_republish.insert(id);
        break;
    case WearRequest::Ban:
        logger::info("{} told never to use {} (banned)", Describe(actor), name);
        TakeOffEverywhere(actor, thing, described, true);
        if (thing->Is(RE::FormType::Spell) || thing->Is(RE::FormType::Shout))
            g_republish.insert(id);
        break;
    case WearRequest::Unban:
        logger::info("{} may use {} again (ban lifted)", Describe(actor), name);
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
            logger::info("{} spell {} asked {} -- record slot {:06X} -- now left {} right {} -- {}", Describe(actor),
                         name, static_cast<int>(hands), slot ? slot->GetFormID() : 0,
                         data.selectedSpells[RE::Actor::SlotTypes::kLeftHand] == spell,
                         data.selectedSpells[RE::Actor::SlotTypes::kRightHand] == spell, CasterState(actor));
            // Look again once time runs, to see what the engine finishes.
            g_republish.insert(id);
        }
        if (thing->Is(RE::FormType::Shout))
        {
            logger::info("{} shout {} -- {}", Describe(actor), name, CasterState(actor));
            g_republish.insert(id);
        }
        break;
    case WearRequest::Unpin:
        // Forgetting the pin is the whole of it, for an item as for a
        // spell: nothing on the thing marks it pinned. (With the
        // prevent-removal flag an item had to come off and go back on
        // without it -- and that path once took a spell off and "put it
        // back" with an item equip, 01:47, Chain Lightning.)
        logger::info("{} told to keep {}{} but not held to it", Describe(actor), name, HandTag(hands));
        break;
    case WearRequest::TakeOff:
        logger::info("{} told to put away {}{}", Describe(actor), name, HandTag(hands));
        UnequipForm(actor, thing, hands, true);
        if (thing->Is(RE::FormType::Spell) || thing->Is(RE::FormType::Shout))
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
}

} // namespace

void RequestWear(ft::ActorId id, std::uint32_t form, WearRequest request, Hand hand)
{
    auto *task = SKSE::GetTaskInterface();
    if (!task)
        return;
    // Queued to the game thread and run there once. The panel is open while
    // this is clicked, and with FreezeTimeOnMenu the tick is held, so the
    // task also republishes her view: the cell answers now rather than when
    // the panel closes.
    task->AddTask([id, form, request, hand]() {
        auto *actor = RE::TESForm::LookupByID<RE::Actor>(id);
        auto *thing = RE::TESForm::LookupByID(form);
        if (!actor || !thing)
            return;
        Wear(actor, thing, request, hand, true);
        PublishFollower(actor);
    });
}

std::vector<ft::PinEntry> PlayerPinsOf(ft::ActorId id)
{
    std::scoped_lock lock(g_pinMutex);
    const auto &book = g_fighting.contains(id) ? g_pinsBeforeFight : g_pins;
    std::vector<ft::PinEntry> entries;
    if (const auto it = book.find(id); it != book.end())
    {
        for (const Pin &pin : it->second)
            entries.push_back({pin.thing.form, pin.hands});
    }
    return entries;
}

void AdoptPins(RE::Actor *actor, const std::vector<ft::PinEntry> &pins)
{
    if (!actor || pins.empty())
        return;
    std::scoped_lock lock(g_pinMutex);
    auto &book = g_pins[actor->GetFormID()];
    for (const ft::PinEntry &entry : pins)
    {
        auto *thing = RE::TESForm::LookupByID(entry.form);
        if (!thing)
        {
            logger::info("{} saved pin {:08X} names nothing in this game -- forgotten", Describe(actor), entry.form);
            continue;
        }
        const char *name = thing->GetName() ? thing->GetName() : "?";
        const Holdable described = DescribeHoldable(actor, thing);
        auto *object = thing->As<RE::TESBoundObject>();
        const bool on = described.IsVoice() ? InVoice(actor, thing) : object && Worn(actor, object, entry.hands);
        if (!on || !Pinnable(described))
        {
            logger::info("{} saved pin on {}{} does not hold -- {} -- forgotten", Describe(actor), name,
                         HandTag(entry.hands), !on ? "not worn now" : "cannot be pinned");
            continue;
        }
        AddPin(book, described, entry.hands, false);
        logger::info("{} saved pin on {}{} taken back", Describe(actor), name, HandTag(entry.hands));
    }
}

void AdoptBans(RE::Actor *actor, const Bans &bans)
{
    if (!actor || bans.empty())
        return;
    std::scoped_lock lock(g_pinMutex);
    auto &book = g_bans[actor->GetFormID()];
    for (const std::uint32_t form : bans)
    {
        const auto *thing = RE::TESForm::LookupByID(form);
        if (!thing)
        {
            logger::info("{} saved ban {:08X} names nothing in this game -- forgotten", Describe(actor), form);
            continue;
        }
        if (Ban(book, form))
            logger::info("{} saved ban on {} taken back", Describe(actor), thing->GetName() ? thing->GetName() : "?");
    }
}

void ForgetPins()
{
    std::scoped_lock lock(g_pinMutex);
    g_pins.clear();
    g_bans.clear();
    g_pinsBeforeFight.clear();
    g_fighting.clear();
    g_refusedLogged.clear();
}

bool PinNow(RE::Actor *actor, std::uint32_t form, Hand hand)
{
    auto *thing = RE::TESForm::LookupByID(form);
    if (!actor || !thing)
        return false;
    // An either-hand thing asked for both hands is pinned once in each; the
    // book's AddPin joins the two. Everything else takes the hands its
    // record gives it, whatever was asked.
    if (hand == Hand::Both && DescribeHoldable(actor, thing).grip == Grip::Either)
    {
        Wear(actor, thing, WearRequest::Pin, Hand::Left, false);
        Wear(actor, thing, WearRequest::Pin, Hand::Right, false);
        return true;
    }
    Wear(actor, thing, WearRequest::Pin, hand, false);
    return true;
}

void ReleaseKind(RE::Actor *actor, Kind kind)
{
    if (!actor)
        return;
    // The pins of that kind, taken out of the book first so the watchdog
    // and the score hook see them gone, then taken off.
    std::vector<Pin> released;
    {
        std::scoped_lock lock(g_pinMutex);
        const auto it = g_pins.find(actor->GetFormID());
        if (it == g_pins.end())
            return;
        std::erase_if(it->second, [&](const Pin &pin) {
            if (pin.thing.kind != kind)
                return false;
            released.push_back(pin);
            return true;
        });
        g_refusedLogged.clear();
    }
    for (const Pin &pin : released)
    {
        auto *thing = RE::TESForm::LookupByID(pin.thing.form);
        if (!thing)
            continue;
        logger::info("{} told to let go of {}{} -- the AI decides again", Describe(actor),
                     thing->GetName() ? thing->GetName() : "?", HandTag(pin.hands));
        UnequipForm(actor, thing, pin.hands, true);
        if (thing->Is(RE::FormType::Spell))
            g_republish.insert(actor->GetFormID());
    }
    if (!released.empty())
        actor->Update3DModel();
}

// --- refusing the engine's equips against the pins -------------------------
//
// Out of combat the engine chooses equipment by a routine nobody has found:
// the best weapon when a fight ends, the default outfit on a cell change, a
// better arrow when one is picked up (docs/RESEARCH.md 7). What it chooses
// is not reachable; what it DOES is, because every choice lands in
// ActorEquipManager::EquipObject, the same function Papyrus EquipItem and
// our own pins call. So the choice is refused where it lands: an equip we
// did not make, of a thing that would take a hand or a slot a pin holds,
// returns without doing anything. The engine tries again, and is refused
// again, silently after the first line. Follower Equip Control has shipped
// this for weapons, shields and ammunition; here it is every pin.
//
// A function-entry detour, through Microsoft Detours: CommonLibSSE's
// trampoline write_branch overwrites an existing jump or call and keeps
// nothing, so it cannot hook a function's first bytes.
namespace
{

using EquipObjectFn = void (*)(RE::ActorEquipManager *, RE::Actor *, RE::TESBoundObject *, RE::ExtraDataList *,
                               std::uint32_t, const RE::BGSEquipSlot *, bool, bool, bool, bool);
EquipObjectFn g_equipObject = nullptr;

// Would this equip, which is not ours, break a pin? The pinned thing itself
// always passes, whichever hand the engine puts it in; the watchdog and
// the score hook see to where it goes.
bool Refused(RE::Actor *actor, RE::TESBoundObject *object, const RE::BGSEquipSlot *slot)
{
    std::scoped_lock lock(g_pinMutex);
    const auto it = g_pins.find(actor->GetFormID());
    const bool anyPins = it != g_pins.end() && !it->second.empty();
    if (const Pin *own = anyPins ? FindPin(it->second, object->GetFormID()) : nullptr)
    {
        // The pinned thing itself passes into its own hand, whichever the
        // engine puts it in; into the OTHER hand only if she has a second
        // copy, or the engine shows the one in both hands.
        const Hand into = SlotHand(slot);
        if (into == Hand::None || Overlap(into, own->hands) || own->hands == Hand::None ||
            CarriedCount(actor, object) >= 2)
            return false;
        if (g_refusedLogged.insert(ReadyKey(actor, object)).second)
            logger::info("{} the engine would equip pinned {} into the {} hand as well, with one copy -- refused",
                         Describe(actor), object->GetName() ? object->GetName() : "?",
                         into == Hand::Left ? "left" : "right");
        return true;
    }
    if (BannedHere(actor->GetFormID(), object->GetFormID()))
    {
        if (g_refusedLogged.insert(ReadyKey(actor, object)).second)
            logger::info("{} the engine would equip banned {} -- refused ({})", Describe(actor),
                         object->GetName() ? object->GetName() : "?",
                         actor->IsInCombat() ? "in combat" : "out of combat");
        return true;
    }
    if (!anyPins)
        return false;
    const std::vector<Pin> &pins = it->second;

    const Holdable thing = DescribeHoldable(actor, object);
    if (thing.kind == Kind::Other)
        return false; // a potion, a scroll: no hand, no slot
    // A bound weapon is the conjuration in progress: refusing it ends the
    // spell she is casting (Follower Equip Control found this the hard
    // way). It passes; the score hook keeps the AI from choosing the spell
    // for a pinned hand in the first place.
    if (const auto *weapon = object->As<RE::TESObjectWEAP>(); weapon && weapon->IsBound())
        return false;
    const Hand hands = HandsFor(thing.grip, SlotHand(slot));
    const bool dualWield = DualWieldAllowed(actor);
    for (const Pin &pin : pins)
    {
        if (!Conflicts(thing, hands, pin.thing, pin.hands, dualWield))
            continue;
        if (g_refusedLogged.insert(ReadyKey(actor, object)).second)
        {
            const auto *held = RE::TESForm::LookupByID(pin.thing.form);
            logger::info("{} the engine would equip {}{} over pinned {}{} -- refused ({})", Describe(actor),
                         object->GetName() ? object->GetName() : "?", HandTag(hands),
                         held && held->GetName() ? held->GetName() : "?", HandTag(pin.hands),
                         actor->IsInCombat() ? "in combat" : "out of combat");
        }
        return true;
    }
    return false;
}

void EquipObjectHook(RE::ActorEquipManager *self, RE::Actor *actor, RE::TESBoundObject *object,
                     RE::ExtraDataList *extra, std::uint32_t count, const RE::BGSEquipSlot *slot, bool queue,
                     bool force, bool sounds, bool applyNow)
{
    if (g_ownEquipDepth == 0 && actor && object && Refused(actor, object, slot))
        return;
    g_equipObject(self, actor, object, extra, count, slot, queue, force, sounds, applyNow);
}

} // namespace

void RefuseEquipsAgainstPins()
{
    // The engine's EquipObject, by address-library id (SE 37938, AE 38894): the
    // same pair the library's own wrapper resolves.
    const REL::Relocation<std::uintptr_t> target{RELOCATION_ID(37938, 38894)};
    g_equipObject = reinterpret_cast<EquipObjectFn>(target.address());

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID &>(g_equipObject), reinterpret_cast<PVOID>(&EquipObjectHook));
    const LONG result = DetourTransactionCommit();
    if (result != NO_ERROR)
    {
        logger::error("pins: could not detour ActorEquipManager::EquipObject (Detours error {}) -- the engine's "
                      "equips will not be refused against the pins",
                      result);
        return;
    }
    logger::info("pins: ActorEquipManager::EquipObject at {:X} detoured -- the engine's equips are refused against "
                 "the pins",
                 target.address());
}

} // namespace ft::game
