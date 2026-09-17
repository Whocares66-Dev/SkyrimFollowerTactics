// Pins: what stays in a hand, and how the promise is kept. The rules are
// core/Loadout.cpp; this is where they meet the engine. Everything here runs
// on the game thread -- the tick, or a task the panel queued.

#include "game/Pins.h"

#include "game/Log.h"
#include "game/Sensors.h"
#include "game/Sheet.h"

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

// Refusals already logged, one per follower, thing and copy (its variant, as
// the log spells it): the engine asks again every frame or so and one line
// says it, but another copy of the same thing is another decision and gets
// its own. Cleared when their pins change, so a fresh conflict is logged
// afresh.
std::unordered_set<std::string> g_refusedLogged;

// Violations already reported: the watchdog finds a pin off, or a banned
// thing on, every half second until the equip takes, and one event says so.
// A pin's by follower, form and hands, since two copies of a form may be
// pinned one per hand; a ban's with no hands. An entry goes when the pin is
// next seen on or the thing off, and a follower's all go when their book
// changes. A handful at most, so a list.
struct ViolationKey
{
    ft::ActorId actor{0};
    std::uint32_t form{0};
    Hand hands{Hand::None};

    [[nodiscard]] bool operator==(const ViolationKey &) const noexcept = default;
};
std::vector<ViolationKey> g_pinsEnforced;
std::vector<ViolationKey> g_bansEnforced;

// True the first time a violation is seen: it is news.
bool FirstReport(std::vector<ViolationKey> &reported, const ViolationKey &key)
{
    for (const ViolationKey &seen : reported)
        if (seen == key)
            return false;
    reported.push_back(key);
    return true;
}

void ClearReport(std::vector<ViolationKey> &reported, const ViolationKey &key)
{
    std::erase(reported, key);
}

void ClearReports(std::vector<ViolationKey> &reported, ft::ActorId actor)
{
    std::erase_if(reported, [actor](const ViolationKey &key) { return key.actor == actor; });
}

// The hand's equip slot record (Sensors.h).
const RE::BGSEquipSlot *HandSlot(Hand hand)
{
    return RE::TESForm::LookupByID<RE::BGSEquipSlot>(hand == Hand::Left ? kLeftHandSlot : kRightHandSlot);
}

// The hand a slot record names. EitherHand and no record are None.
Hand SlotHand(const RE::BGSEquipSlot *slot)
{
    switch (slot ? slot->GetFormID() : 0)
    {
    case kRightHandSlot:
        return Hand::Right;
    case kLeftHandSlot:
        return Hand::Left;
    case kBothHandsSlot:
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

// The rules themselves are in core/Loadout.cpp, where they are tested.
// An equip or an unequip hands the engine the copy's own extra list, as
// SKSE's EquipItemEx and UnequipItemEx do (WornList, UnwornList): an
// equip handed no list is the engine's to resolve, and a second copy going
// into the other hand wants the spare one.
Holdable DescribeHoldable(RE::Actor *actor, RE::TESForm *form, const std::optional<ft::ItemVariant> &variant)
{
    Holdable thing;
    thing.form = form->GetFormID();
    thing.variant = variant;
    if (auto *weapon = form->As<RE::TESObjectWEAP>())
    {
        thing.kind = Kind::Weapon;
        thing.grip = TwoHanded(weapon) ? Grip::Both : Grip::Either;
        // The variant's count, for the one-copy rule: the one tempered
        // dagger has no second copy for the other hand though three plain
        // ones are in the bag; the form's, whichever variant.
        thing.count = CountVariant(actor, weapon, variant);
    }
    else if (auto *spell = form->As<RE::SpellItem>())
    {
        const auto type = spell->GetSpellType();
        if (IsPower(spell))
        {
            thing.kind = Kind::Voice; // readied in the voice slot, no hand
            return thing;
        }
        if (type != RE::MagicSystem::SpellType::kSpell)
            return thing; // an ability, a disease: nothing to hold
        thing.kind = Kind::Spell;
        // Never short of copies: a spell can be in both hands at once, and
        // the one-copy rule (KeptFromAI) must not keep it out of the other
        // hand as it keeps a lone dagger. Left at 1 until 2026-09-11, it
        // did exactly that, the opposite of what the tests prove.
        thing.count = 2;
        const auto *slot = spell->GetEquipSlot();
        const std::uint32_t slotId = slot ? slot->GetFormID() : 0;
        thing.grip = spell->IsTwoHanded()       ? Grip::Both
                     : slotId == kLeftHandSlot  ? Grip::LeftOnly
                     : slotId == kRightHandSlot ? Grip::RightOnly
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
    else if (auto *ammo = form->As<RE::TESAmmo>())
    {
        thing.kind = Kind::Ammo;
        thing.damage = ammo->GetRuntimeData().data.damage;
    }
    else if (form->Is(RE::FormType::Shout))
    {
        thing.kind = Kind::Voice;
    }
    return thing;
}

// Is this form readied in the voice slot: a power or a shout the actor has
// selected? Voice things have no hand; this is their "equipped".
bool EquipSpellIn(RE::Actor *actor, RE::SpellItem *spell, Hand hand);

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
    const auto name = [](const RE::MagicItem *spell) { return NameOr(spell, "-"); };
    const auto *left = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kLeftHand);
    const auto *right = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kRightHand);
    const auto *voice = data.selectedPower;
    return std::string("selected L=") + name(data.selectedSpells[RE::Actor::SlotTypes::kLeftHand]) +
           " R=" + name(data.selectedSpells[RE::Actor::SlotTypes::kRightHand]) + " voice=" + (NameOr(voice, "-")) +
           " -- caster L=" + name(left ? left->currentSpell : nullptr) +
           " R=" + name(right ? right->currentSpell : nullptr);
}

void UnequipForm(RE::Actor *actor, RE::TESForm *form, Hand hands, bool now,
                 const std::optional<ft::ItemVariant> &variant = std::nullopt);

void EquipPinned(RE::Actor *actor, RE::TESForm *form, Hand hands, bool now,
                 const std::optional<ft::ItemVariant> &variant, RE::ExtraDataList *row = nullptr, bool exact = false)
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
            manager->EquipSpell(actor, spell, RE::TESForm::LookupByID<RE::BGSEquipSlot>(kVoiceSlot));
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
    //
    // Which copy goes is the caller's kind of question. The panel is exact:
    // the row clicked -- its list, or the plain stack's own copies, a list
    // of the stack or a listless copy by null -- and a copy of that row
    // already in the hand is the incumbent, nothing done; one in the other
    // hand comes across. A rule or the watchdog means the variant, and has
    // the leeway: a copy of it already in the hand is the incumbent; else
    // one of the stack before a row of its own (the poisoned dagger shares
    // the plain variant), a listless copy by null, and the only copy in the
    // other hand brought across. The engine leaves a worn list where it is,
    // whichever hand is asked (measured 2026-09-12), so a copy that comes
    // across is taken off there first. A row token is matched against the
    // bag's lists by address and never dereferenced unmatched: the copy may
    // have left since the scan.
    const bool oneHand = object->IsWeapon() && (hands == Hand::Left || hands == Hand::Right);
    const Hand other = Without(Hand::Both, hands);
    RE::ExtraDataList *list = nullptr;
    bool listless = false; // a copy with no list: null names it
    if (exact && row)
    {
        list = ListOfAddress(actor, object, row);
        if (!list || WornIn(object, list, hands))
            return;
        if (ListWorn(list, Hand::None))
        {
            if (!oneHand)
                return;
            UnequipForm(actor, form, other, now, variant);
        }
    }
    else if (exact)
    {
        if (WornStackList(actor, object, hands))
            return;
        list = UnwornStackList(actor, object);
        if (!list && HasListlessCopy(actor, object))
            listless = true;
        if (!list && !listless)
        {
            if (!oneHand)
                return;
            list = WornStackList(actor, object, other);
            if (!list)
                return;
            UnequipForm(actor, form, other, now, variant);
        }
    }
    else
    {
        const bool worn =
            variant ? WornVariantList(actor, object, *variant, hands) != nullptr : EquippedIn(actor, object, hands);
        if (worn)
            return;
        list = variant ? UnwornVariantList(actor, object, *variant) : UnwornList(actor, object);
        if (list && variant && variant->IsPlain() && RowOfItsOwn(list) && HasListlessCopy(actor, object))
        {
            list = nullptr;
            listless = true;
        }
        if (!list && !listless && oneHand && CountVariant(actor, object, variant) < 2)
        {
            list = variant ? WornVariantList(actor, object, *variant, other) : WornList(actor, object, other);
            if (list)
                UnequipForm(actor, form, other, now, variant);
        }
        if (!list && !listless && variant && !(variant->IsPlain() && HasListlessCopy(actor, object)))
            return;
    }
    log::pins.debug("{} equip {}{}: list [{}]{}", Describe(actor), log::NameOf(form), HandTag(hands), ListEntries(list),
                    row ? (list == row ? " (the row clicked)" : " (the row clicked is gone)") : "");
    manager->EquipObject(actor, object, list, 1, slot, !now, false, true, false);
    if (now && log::Enabled(log::Level::Debug))
    {
        const Carried carried = CarriedOf(actor, object);
        std::string lists;
        if (carried.entry && carried.entry->extraLists)
            for (const auto *each : *carried.entry->extraLists)
                lists += (lists.empty() ? "[" : " [") + ListEntries(each) + "]";
        log::pins.debug("{} after the equip, {} x{}: {}", Describe(actor), log::NameOf(form), carried.count,
                        lists.empty() ? "no lists" : lists);
    }
}

// Take a form off.
//
// Items go through the equip manager WITHOUT the prevent-equip flag: the
// Creation Kit wiki notes that flag does nothing for weapons on an NPC and
// works only too well for ammunition, leaving an archer holding a bow they
// cannot use. A spell goes off through the Papyrus native
// Actor.UnequipSpell(spell, source) -- 0 for the left hand, 1 for the
// right, 2 for the voice -- dispatched to the script VM, which runs it on
// the game thread A FRAME LATER. That lateness is why a click taking a
// spell off asks the panel to look again on its next frame
// (RefreshShownPageSoon, game/Tactics.h): the build straight after the
// click still reads the spell in hand.
//
// It is not for want of a native, as this comment claimed until
// 2026-09-17. `Actor::DeselectSpell` (RELOCATION_ID(37820, 38769)) does the
// work and does it at once: read off 1.6.1170, it walks the four selected
// spell slots, nulls any holding the spell and tells that slot's caster,
// then clears selectedPower where what sits there is a SpellItem (form type
// 0x16). Two things keep us on Papyrus for now. It takes no hand, so it
// clears the spell from BOTH hands where this unequips only the hand asked,
// which is a difference per-hand pins care about; and that form-type gate
// leaves a shout in the voice alone, so UnequipShout is a Papyrus call
// whatever is done here. Neither tried in play.
// Is the thing on anywhere it could be, and off from everywhere it is. An
// either-hand thing is asked about, and taken from, each hand in turn: the
// item code reads "both hands" as a two-hander's, which lives in the right,
// and a ban on a dagger in the left hand found nothing to take off (16:27,
// Marcurio's iron dagger).
bool OnAnywhere(RE::Actor *actor, RE::TESForm *form, const Holdable &described);
void TakeOffEverywhere(RE::Actor *actor, RE::TESForm *form, const Holdable &described, bool now);

// Is the item on, in a hand or worn? The no-op check before an unequip.
// A variant is asked of its rows' lists; the form, of any copy.
bool Worn(RE::Actor *actor, RE::TESBoundObject *object, Hand hands,
          const std::optional<ft::ItemVariant> &variant = std::nullopt)
{
    if (variant)
        return WornVariantList(actor, object, *variant, hands) != nullptr;
    if (hands != Hand::None)
        return EquippedIn(actor, object, hands);
    const Carried carried = CarriedOf(actor, object);
    return carried.entry && carried.entry->IsWorn();
}

// The engine's own unequips, which CommonLib declares for neither spell nor
// shout. The Papyrus natives behind Actor.UnequipSpell and
// Actor.UnequipShout are thin wrappers: they null-check, and tail-call
// these on the equip manager singleton (read off 1.6.1170 --
// docs/COMMONLIB.md has the trace, docs/VERSIONS.md the IDs). Calling them
// here does on THIS frame what the Papyrus dispatch does on the next.
//
// AE only, as the Special Edition half of each ID was never read: on SE and
// VR these answer false and the Papyrus route runs instead, a frame late
// but right.
bool UnequipSpellNow(RE::Actor *actor, RE::SpellItem *spell, std::uint32_t source)
{
    auto *manager = actor && spell && REL::Module::IsAE() ? RE::ActorEquipManager::GetSingleton() : nullptr;
    if (!manager)
        return false;
    // (manager, actor, spell, source), the source being Papyrus's aiSource
    // passed straight through: 0 the left hand, 1 the right, 2 the voice.
    // The function turns it into the matching equip slot itself.
    using func_t = void (*)(RE::ActorEquipManager *, RE::Actor *, RE::SpellItem *, std::uint32_t);
    static REL::Relocation<func_t> func{REL::ID(38903)};
    func(manager, actor, spell, source);
    return true;
}

bool UnequipShoutNow(RE::Actor *actor, RE::TESShout *shout)
{
    auto *manager = actor && shout && REL::Module::IsAE() ? RE::ActorEquipManager::GetSingleton() : nullptr;
    if (!manager)
        return false;
    using func_t = void (*)(RE::ActorEquipManager *, RE::Actor *, RE::TESShout *);
    static REL::Relocation<func_t> func{REL::ID(38904)};
    func(manager, actor, shout);
    return true;
}

// And the Papyrus route the two fall back to, which the VM runs on the game
// thread a frame later. Written once here rather than twice below: the hand
// spells and the voice ask for the same call with a different source.
//
// MakeFunctionArguments takes Args&&, and an lvalue pointer deduces a
// reference type that fails its is_return_convertible gate -- so the
// std::move is the call's shape, not a copy avoided.
void DispatchUnequipSpell(RE::Actor *actor, RE::SpellItem *spell, std::uint32_t source)
{
    auto *vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    auto *policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
    if (!policy)
        return;
    const auto handle = policy->GetHandleForObject(actor->GetFormType(), actor);
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;
    // A block and not a NOLINTNEXTLINE: the formatter wraps this call, and
    // the move then sits a line below the one the suppression covers, which
    // is how it was caught by the linter rather than by reading (2026-09-17).
    // NOLINTBEGIN(performance-move-const-arg)
    vm->DispatchMethodCall2(handle, "Actor", "UnequipSpell",
                            RE::MakeFunctionArguments(std::move(spell), static_cast<std::int32_t>(source)), result);
    // NOLINTEND(performance-move-const-arg)
}

void DispatchUnequipShout(RE::Actor *actor, RE::TESShout *shout)
{
    auto *vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    auto *policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
    if (!policy)
        return;
    const auto handle = policy->GetHandleForObject(actor->GetFormType(), actor);
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;
    // A block here too: this call fits on one line today, and would lose its
    // suppression the moment it did not.
    // NOLINTBEGIN(performance-move-const-arg)
    vm->DispatchMethodCall2(handle, "Actor", "UnequipShout", RE::MakeFunctionArguments(std::move(shout)), result);
    // NOLINTEND(performance-move-const-arg)
}

// Take a spell out of a hand, or an item off. A no-op when it is not
// there, like EquipSpellIn: a spell's unequip is a native or a Papyrus
// call and a republish, and neither is owed for a hand that was already
// empty.
void UnequipForm(RE::Actor *actor, RE::TESForm *form, Hand hands, bool now,
                 const std::optional<ft::ItemVariant> &variant)
{
    // The voice: Papyrus's UnequipShout for a shout, UnequipSpell with the
    // voice source (2) for a power. Neither has a native in CommonLibSSE,
    // as a hand spell's unequip has not.
    if (DescribeHoldable(actor, form).IsVoice())
    {
        if (!InVoice(actor, form))
            return;
        // The voice is source 2 for a power; a shout has a call of its own.
        if (auto *shout = form->As<RE::TESShout>())
        {
            if (!UnequipShoutNow(actor, shout))
                DispatchUnequipShout(actor, shout);
        }
        else if (auto *power = form->As<RE::SpellItem>())
        {
            if (!UnequipSpellNow(actor, power, 2))
                DispatchUnequipSpell(actor, power, 2);
        }
        return;
    }
    if (auto *spell = form->As<RE::SpellItem>())
    {
        for (const Hand hand : {Hand::Left, Hand::Right})
        {
            if (!Overlap(hands, hand) || !EquippedIn(actor, spell, hand))
                continue;
            const std::uint32_t source = hand == Hand::Left ? 0 : 1;
            if (!UnequipSpellNow(actor, spell, source))
                DispatchUnequipSpell(actor, spell, source);
        }
        return;
    }
    if (auto *object = form->As<RE::TESBoundObject>())
    {
        if (!Worn(actor, object, hands, variant))
            return;
        // A one-handed weapon comes out of the hand named; anything else
        // out of wherever it is.
        const RE::BGSEquipSlot *slot = nullptr;
        if (object->Is(RE::FormType::Weapon) && (hands == Hand::Left || hands == Hand::Right))
            slot = HandSlot(hands);
        RE::ExtraDataList *list =
            variant ? WornVariantList(actor, object, *variant, hands) : WornList(actor, object, hands);
        if (auto *manager = RE::ActorEquipManager::GetSingleton())
            manager->UnequipObject(actor, object, list, 1, slot, !now, false, true, false, nullptr);
    }
}

bool OnAnywhere(RE::Actor *actor, RE::TESForm *form, const Holdable &described)
{
    if (described.IsVoice())
        return InVoice(actor, form);
    if (described.variant)
    {
        auto *object = form->As<RE::TESBoundObject>();
        return object && Worn(actor, object, Hand::None, described.variant);
    }
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
        UnequipForm(actor, form, Hand::Left, now, described.variant);
        UnequipForm(actor, form, Hand::Right, now, described.variant);
        return;
    }
    UnequipForm(actor, form, described.grip == Grip::None ? Hand::None : Reach(described.grip), now, described.variant);
}

// An item as a pin or ban event names it: form and name, the hand, which
// copy, and who changed the book -- player, rule, fight-end, save, game --
// where someone did (nullptr for the watchdog putting a promise back).
std::vector<log::Field> ItemFields(std::uint32_t form, const std::optional<ft::ItemVariant> &variant, Hand hands,
                                   const char *by)
{
    std::vector<log::Field> fields;
    log::AppendForm(fields, "itemFormId", "itemName", form);
    fields.emplace_back("hand", HandTag(hands));
    fields.emplace_back("variant", ft::VariantText(variant));
    if (by)
        fields.emplace_back("by", by);
    return fields;
}

// Before something new goes on, whatever it displaces is unpinned, so the
// book and the body agree: the engine's own displacement would leave the
// old pin in the book, and the watchdog would put it straight back over the
// new thing. Taking it off is the engine's, in the equip that follows.
// A request applied to the book (core's ApplyRequest), and what gave way
// logged. Only the book changes here. What gave way is NOT taken off: the
// engine's equip displaces it -- a weapon or spell from the hand it takes,
// armour from shared body slots, arrows from the quiver, a power or shout
// from the voice -- so the explicit unequip that used to follow did
// nothing for items and spells and undid the new equip for the voice (its
// spell unequip is a deferred Papyrus call, which landed a frame after the
// new power was in and emptied the slot, 2026-09-05). The unequip was
// needed while pinned items carried the engine's prevent-removal flag,
// which refused the engine's own swap; the flag went on 2026-09-04, and
// this went with it. The one unequip that stays is the move of a
// follower's only weapon to the other hand, in Wear. `before` is the book
// remembered for after the fight, given when a rule pins in one: a pin
// displaced from it is the player's, overridden for the fight rather than
// released.
void ApplyToBook(RE::Actor *actor, std::vector<Pin> &pins, PinRequest request, const Holdable &incoming, Hand hands,
                 bool moving, bool dualWield, const char *by, const std::vector<Pin> *before)
{
    const char *const why = request == PinRequest::Ban ? "banned" : "to make room";
    for (const Displaced &gone : ApplyRequest(pins, request, incoming, hands, moving, dualWield))
    {
        const std::string name = log::NameOf(RE::TESForm::LookupByID(gone.form));
        std::vector<log::Field> fields = ItemFields(gone.form, gone.variant, gone.hands, by);
        if (before && FindPin(*before, gone.form, gone.variant))
        {
            log::AppendForm(fields, "overriddenByFormId", "overriddenByName", incoming.form);
            log::pins.event(log::Level::Info, "pin.overridden", actor, fields,
                            "{} unpinning {}{} for the fight: a rule pins {} over it", Describe(actor), name,
                            HandTag(gone.hands), log::NameOf(RE::TESForm::LookupByID(incoming.form)));
            continue;
        }
        fields.emplace_back("reason", why);
        log::pins.event(log::Level::Info, "pin.released", actor, fields, "{} unpinning {}{} -- {}", Describe(actor),
                        name, HandTag(gone.hands), why);
    }
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
    const auto name = [](const RE::TESForm *form) { return NameOr(form, "-"); };
    return CasterState(actor) + " -- held L=" + name(actor->GetEquippedObject(true)) +
           " R=" + name(actor->GetEquippedObject(false));
}

// The copy an equip is judged as, for the log: its variant as the events
// spell it, or that the engine was left to pick one.
std::string CopyText(const std::optional<ft::ItemVariant> &variant)
{
    return variant ? ft::VariantText(variant) : std::string("no list named");
}

// What is where a put-back pin goes, for the log: the voice's power or shout,
// or what the pinned hand holds (the right, for both hands). Nothing for
// armour, whose slots this does not read.
std::uint32_t DisplacedBy(RE::Actor *actor, const Pin &pin)
{
    const RE::TESForm *there = nullptr;
    if (pin.thing.IsVoice())
        there = actor->GetActorRuntimeData().selectedPower;
    else if (pin.hands == Hand::Left)
        there = actor->GetEquippedObject(true);
    else if (pin.hands != Hand::None)
        there = actor->GetEquippedObject(false);
    return there ? there->GetFormID() : 0;
}

// A pin found off and put back. The first time for this violation it is
// pin.enforced; the repeats, while the equip does not take, go to debug,
// where a line that keeps coming says the equip detour has missed a path,
// and the hand state beside it says which.
void ReportEnforced(RE::Actor *actor, const Pin &pin, RE::TESForm *form, bool first, const char *reason)
{
    if (!first)
    {
        log::pins.debug("{} pinned {}{} still off ({}) -- putting it back again -- {}", Describe(actor),
                        log::NameOf(form), HandTag(pin.hands), reason, HandsState(actor));
        return;
    }
    std::vector<log::Field> fields = ItemFields(pin.thing.form, pin.thing.variant, pin.hands, nullptr);
    log::AppendForm(fields, "displacedByFormId", "displacedByName", DisplacedBy(actor, pin));
    fields.emplace_back("reason", reason);
    log::pins.event(log::Level::Info, "pin.enforced", actor, fields, "{} pinned {}{} {} -- putting it back -- {}",
                    Describe(actor), log::NameOf(form), HandTag(pin.hands), reason, HandsState(actor));
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
// (21:02). Under g_pinMutex. Returns what it let go and pinned again, for
// the rest of the watchdog's pass.
AfterFight RestorePinsAfterFight(RE::Actor *actor, std::vector<Pin> &pins, const std::vector<Pin> &before)
{
    const AfterFight settle = SettleAfterFight(pins, before);
    for (const Released &gone : settle.released)
    {
        auto *thing = RE::TESForm::LookupByID(gone.form);
        if (!thing)
            continue;
        const std::string name = NameOr(thing, "?");
        if (gone.takeOff)
        {
            std::vector<log::Field> fields = ItemFields(gone.form, gone.variant, gone.hands, "fight-end");
            fields.emplace_back("reason", "pinned in the fight; what was there before comes back");
            log::pins.event(log::Level::Info, "pin.released", actor, fields,
                            "{} fight over -- {}{} pinned during it comes off; what was there before comes "
                            "back",
                            Describe(actor), name, HandTag(gone.hands));
            UnequipForm(actor, thing, gone.hands, true, gone.variant);
            continue;
        }
        // Forgetting the pin is the whole of it: nothing on the item marks
        // it pinned, so there is nothing to lift.
        std::vector<log::Field> fields = ItemFields(gone.form, gone.variant, gone.hands, "fight-end");
        fields.emplace_back("reason", "pinned in the fight; the item stays on");
        log::pins.event(log::Level::Info, "pin.released", actor, fields,
                        "{} fight over -- {}{} pinned during it stays on, unpinned", Describe(actor), name,
                        HandTag(gone.hands));
    }
    for (const Pin &pin : settle.restored)
    {
        const auto *thing = RE::TESForm::LookupByID(pin.thing.form);
        log::pins.event(log::Level::Info, "pin.restored", actor,
                        ItemFields(pin.thing.form, pin.thing.variant, pin.hands, "fight-end"),
                        "{} fight over -- {}{} pinned again, as before it", Describe(actor), log::NameOf(thing),
                        HandTag(pin.hands));
    }
    pins = before;
    if (settle.released.empty() && settle.restored.empty())
        return settle;
    // A fresh start for the refusal log: the book is what it was.
    g_refusedLogged.clear();
    actor->Update3DModel();
    return settle;
}

// Note a follower entering or leaving combat, and return what the end of a
// fight let go and pinned again. Under g_pinMutex.
AfterFight NoteFight(RE::Actor *actor, std::vector<Pin> &pins, bool fighting)
{
    const ft::ActorId id = actor->GetFormID();
    const bool was = g_fighting.contains(id);
    if (fighting && !was)
    {
        g_fighting.insert(id);
        g_pinsBeforeFight[id] = pins;
        if (!pins.empty())
            log::pins.debug("{} fight begins -- {} pin(s) remembered for after it", Describe(actor), pins.size());
        return {};
    }
    if (!fighting && was)
    {
        g_fighting.erase(id);
        if (auto saved = g_pinsBeforeFight.extract(id); !saved.empty())
            return RestorePinsAfterFight(actor, pins, saved.mapped());
    }
    return {};
}

void EnforcePins(const std::vector<RE::Actor *> &followers)
{
    // The book is read and changed under the lock; the engine is acted on
    // after it, as ReleaseKind does: an equip re-enters the equip detour,
    // and the combat AI's scoring takes the same lock.
    struct Deferred
    {
        RE::Actor *actor;
        RE::TESForm *form;
        Hand hands;
        bool takeOff;
        Holdable described;
        std::optional<ft::ItemVariant> variant;
    };
    std::vector<Deferred> todo;
    // Per follower, the forms the end of a fight let go: a banned thing a
    // rule had pinned for the fight is taken off below, and that is the
    // fight ending, not a promise broken.
    std::unordered_map<ft::ActorId, std::vector<std::uint32_t>> lapsed;

    std::unique_lock lock(g_pinMutex);

    for (auto *actor : followers)
    {
        // Every follower, pins or none: a fight that begins with an empty
        // book and ends with a rule's pin in it still has to be put right.
        const ft::ActorId id = actor->GetFormID();
        auto &pins = g_pins[id];
        const bool fighting = actor->IsInCombat();
        const AfterFight settled = NoteFight(actor, pins, fighting);
        // What the end of a fight pinned again goes back on in this same
        // pass, and was reported as pin.restored: counted as reported, so
        // putting it back is not reported again as a violation.
        for (const Pin &restored : settled.restored)
            FirstReport(g_pinsEnforced, {id, restored.thing.form, restored.hands});
        for (const Released &gone : settled.released)
            lapsed[id].push_back(gone.form);
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
            const ViolationKey key{id, pin->thing.form, hands};

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
                const bool on = InVoice(actor, form);
                if (on)
                    ClearReport(g_pinsEnforced, key);
                if (PutBackNow(*pin, on, fighting, casting))
                {
                    ReportEnforced(actor, *pin, form, FirstReport(g_pinsEnforced, key), "put away from the voice");
                    todo.push_back({actor, form, hands, false, {}, std::nullopt});
                }
            }
            else if (form->Is(RE::FormType::Spell))
            {
                const bool on = EquippedIn(actor, form, hands);
                if (on)
                    ClearReport(g_pinsEnforced, key);
                if (PutBackNow(*pin, on, fighting, casting))
                {
                    ReportEnforced(actor, *pin, form, FirstReport(g_pinsEnforced, key), "put away");
                    todo.push_back({actor, form, hands, false, {}, std::nullopt});
                }
            }
            else if (auto *object = form->As<RE::TESBoundObject>())
            {
                // A pin on a variant asks after its rows: none left, the
                // pin goes; on, by a row of the variant worn where the pin
                // says, not the form's.
                const std::optional<ft::ItemVariant> &variant = pin->thing.variant;
                const bool gone = CountVariant(actor, object, variant) <= 0;
                if (gone)
                {
                    std::vector<log::Field> fields = ItemFields(object->GetFormID(), variant, hands, "game");
                    fields.emplace_back("reason", "no longer carried");
                    log::pins.event(log::Level::Info, "pin.released", actor, fields,
                                    "{} no longer carries {} -- pin dropped", Describe(actor), log::NameOf(object));
                    ClearReport(g_pinsEnforced, key);
                    pin = pins.erase(pin);
                    continue;
                }
                const bool on = Worn(actor, object, hands, variant);
                if (on)
                    ClearReport(g_pinsEnforced, key);
                if (PutBackNow(*pin, on, fighting, casting))
                {
                    ReportEnforced(actor, *pin, object, FirstReport(g_pinsEnforced, key), "taken off");
                    todo.push_back({actor, object, hands, false, {}, variant});
                }
            }
            ++pin;
        }
    }

    std::erase_if(g_pins, [](const auto &entry) { return entry.second.empty(); });

    // A banned thing found on comes off -- unless a pin holds it (a rule's
    // instruction, the player's own, wins for as long as it lasts) or one
    // of our casts has the hand. The score hook and the equip detour keep
    // this from happening; this is what answers it when it has. After the
    // pin pass on purpose: the tick a fight ends, NoteFight above lets the
    // rules' pins go, and a banned thing a rule pinned for the fight is
    // found here unpinned and taken off in the same pass.
    for (auto *actor : followers)
    {
        const auto it = g_bans.find(actor->GetFormID());
        if (it == g_bans.end() || it->second.empty())
            continue;
        const auto pinsIt = g_pins.find(actor->GetFormID());
        const std::vector<Pin> *pins = pinsIt == g_pins.end() ? nullptr : &pinsIt->second;
        if (IsMidCast(actor))
            continue;
        for (auto ban = it->second.begin(); ban != it->second.end();)
        {
            auto *thing = RE::TESForm::LookupByID(ban->form);
            if (!thing)
            {
                ++ban;
                continue;
            }
            const ViolationKey key{actor->GetFormID(), ban->form, Hand::None};
            // A ban on a variant holds while a row of it is in the bag,
            // and goes when none is: it has nothing left to promise about.
            if (ban->variant)
            {
                auto *object = thing->As<RE::TESBoundObject>();
                if (object && CountVariant(actor, object, ban->variant) <= 0)
                {
                    std::vector<log::Field> fields = ItemFields(ban->form, ban->variant, Hand::None, "game");
                    fields.emplace_back("reason", "no longer carried");
                    log::pins.event(log::Level::Info, "ban.released", actor, fields,
                                    "{} no longer carries {} -- ban dropped", Describe(actor), log::NameOf(thing));
                    ClearReport(g_bansEnforced, key);
                    ban = it->second.erase(ban);
                    continue;
                }
            }
            const Holdable described = DescribeHoldable(actor, thing, ban->variant);
            const bool pinned = pins && FindPin(*pins, described);
            if (pinned || !OnAnywhere(actor, thing, described))
            {
                ClearReport(g_bansEnforced, key);
                ++ban;
                continue;
            }
            if (FirstReport(g_bansEnforced, key))
            {
                Hand foundIn = Hand::None;
                for (const Hand hand : {Hand::Left, Hand::Right})
                    if (EquippedIn(actor, thing, hand))
                        foundIn = foundIn | hand;
                bool afterFight = false;
                if (const auto lapsedHere = lapsed.find(actor->GetFormID()); lapsedHere != lapsed.end())
                    for (const std::uint32_t form : lapsedHere->second)
                        afterFight = afterFight || form == ban->form;
                log::pins.event(
                    log::Level::Info, "ban.enforced", actor,
                    ItemFields(thing->GetFormID(), ban->variant, foundIn, afterFight ? "fight-end" : nullptr),
                    "{} has banned {}{} on{} -- taking it off", Describe(actor), log::NameOf(thing), HandTag(foundIn),
                    afterFight ? ", a rule's pin on it gone with the fight" : "");
            }
            todo.push_back({actor, thing, Hand::None, true, described, ban->variant});
            ++ban;
        }
    }

    lock.unlock();
    for (const Deferred &d : todo)
    {
        if (d.takeOff)
            TakeOffEverywhere(d.actor, d.form, d.described, false);
        else
            EquipPinned(d.actor, d.form, d.hands, false, d.variant);
    }
}

// --- keeping the AI to the pins ------------------------------------------
//
// The combat AI chooses from a list of its own, the combat inventory it
// builds when a fight begins: spells and items together, scored, in seven
// arrays by role. It does not read their spell lists or their bag again during
// the fight -- Firebolt was cast after being removed from their record, a
// removed dagger never was. So a pin is kept by answering THAT list's
// scoring, below: an entry that would take a pinned hand scores zero when
// the AI asks. Nothing of theirs changes, nothing is saved, and the engine
// discards the list when the fight ends, so there is nothing to restore.
// Two earlier ways were dropped: removing competing spells from their record
// for the life of a pin (it worked, and left them without those for every
// menu, script and mod in between), and erasing entries from the list on
// each tick (a race the AI won; it re-lists every few seconds).

// What the combat AI is choosing from: its combat inventory, seven arrays
// of scored options built for the fight. Logged once per fight, by name,
// to learn the layout -- the AI cast a spell we had removed from their lists
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
// alone, whatever hand the entry is for. The hook runs for EVERY actor the
// combat AI scores for, so an actor with no pins and no bans -- every
// creature in the cell, and a follower nobody has touched -- is answered
// with no more than the lock and two map lookups; the record and the
// inventory are only read for the few with a book.
bool ShadowedEntry(RE::CombatInventoryItem *entry, RE::Actor *actor, const char *&why)
{
    // A flag read, ahead of the lock: out of the player's service the book
    // waits for them to rejoin (Pins.h).
    if (!entry || !entry->item || !actor || !actor->IsPlayerTeammate())
        return false;
    std::vector<Pin> pins;
    Bans bans;
    {
        std::scoped_lock lock(g_pinMutex);
        if (const auto it = g_pins.find(actor->GetFormID()); it != g_pins.end())
            pins = it->second;
        if (const auto it = g_bans.find(actor->GetFormID()); it != g_bans.end())
            bans = it->second;
    }
    if (pins.empty() && bans.empty())
        return false;
    // The rule is core's (ShadowOf, tested); this reads what it asks for.
    // The other hand is read here because the AI's entry for a staff --
    // a weapon it handles as magic, listed per hand as a spell is, with no
    // count behind it -- would otherwise put Jenassa's one Staff of Flames
    // in both hands (18:56).
    const Holdable thing = DescribeHoldable(actor, entry->item);
    const Hand slot = SlotHand(entry->itemSlot.equipSlot);
    auto *object = entry->item->As<RE::TESBoundObject>();
    const std::vector<ItemVariant> rows = object ? RowsOf(actor, object) : std::vector<ItemVariant>{};
    const bool heldInOtherHand =
        (slot == Hand::Left || slot == Hand::Right) && actor->GetEquippedObject(slot != Hand::Left) == entry->item;
    switch (ShadowOf(pins, bans, thing, slot, rows, heldInOtherHand))
    {
    case Shadow::Banned:
        why = "banned";
        return true;
    case Shadow::OnlyOneInOtherHand:
        why = "the only one, in the other hand";
        return true;
    case Shadow::PinnedAgainst:
        why = "pinned against";
        return true;
    default:
        return false;
    }
}

// The originals are looked up and the once-set touched under the pin
// mutex: the AI scores on its own schedule, and ProbeCombatInventory adds
// classes and clears the set from the tick.
float ScoreHook(RE::CombatInventoryItem *self, RE::CombatController *controller)
{
    const auto vtable = *reinterpret_cast<const std::uintptr_t *>(self);
    ScoreFn originalFn = nullptr;
    {
        std::scoped_lock lock(g_pinMutex);
        const auto original = g_scoreOriginals.find(vtable);
        if (original != g_scoreOriginals.end())
            originalFn = original->second;
    }
    // Cannot happen -- a class's original is recorded before its slot is
    // written -- and if it does the entry keeps its last score rather than
    // being silently scored 0 for everyone.
    if (!originalFn)
        return self->itemScore;
    const float score = originalFn(self, controller);
    const RE::NiPointer<RE::Actor> actor = AttackerOf(controller);
    const char *why = "";
    if (!ShadowedEntry(self, actor.get(), why))
        return score;
    bool first = false;
    {
        std::scoped_lock lock(g_pinMutex);
        first = g_zeroedOnce.insert(self).second;
    }
    if (first)
    {
        log::pins.debug("{} AI asked the score of {}{}: {:.2f}, answered 0 ({})", Describe(actor.get()),
                        log::NameOf(self->item), HandTag(SlotHand(self->itemSlot.equipSlot)), score, why);
    }
    return 0.0f;
}

void WatchScoresIn(std::uintptr_t vtable, const char *what)
{
    std::scoped_lock lock(g_pinMutex);
    if (g_scoreOriginals.contains(vtable))
        return;
    // The original is recorded BEFORE the slot is written, so a call that
    // lands between the two finds it.
    REL::Relocation<std::uintptr_t> table{vtable};
    const auto slot = table.address() + kCalculateScoreSlot * sizeof(std::uintptr_t);
    g_scoreOriginals[vtable] = reinterpret_cast<ScoreFn>(*reinterpret_cast<std::uintptr_t *>(slot));
    table.write_vfunc(kCalculateScoreSlot, ScoreHook);
    log::pins.debug("watching the AI's score of {} (vtable {:X})", what, vtable);
}

void WatchScoreOf(RE::CombatInventoryItem *entry)
{
    if (!entry)
        return;
    const auto vtable = *reinterpret_cast<const std::uintptr_t *>(entry);
    {
        std::scoped_lock lock(g_pinMutex);
        if (g_scoreOriginals.contains(vtable))
            return;
    }
    // A class the load-time table did not name: say so, with the entry's
    // last score as a check that the slot is the scoring one.
    log::pins.debug("an AI entry class not in the table: entries like {} (last score {:.2f})", log::NameOf(entry->item),
                    entry->itemScore);
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
    {
        std::scoped_lock lock(g_pinMutex);
        g_zeroedOnce.clear();
    }
    // Every class of entry gets its score watched; the names are built
    // only when the log would take them.
    const bool logging = log::Enabled(log::Level::Debug);
    std::unordered_set<const RE::TESForm *> listed;
    for (int slot = 0; slot < 7; ++slot)
    {
        std::string names;
        for (const auto &entry : controller->inventory->inventoryItems[slot])
        {
            const auto *form = entry ? entry->item : nullptr;
            listed.insert(form);
            if (logging)
                names += (names.empty() ? "" : ", ") + std::string(NameOr(form, "?")) +
                         HandTag(entry ? SlotHand(entry->itemSlot.equipSlot) : Hand::None);
            WatchScoreOf(entry.get());
        }
        if (logging)
            log::pins.debug("{} combat inventory [{}]: {}", Describe(actor), slot, names.empty() ? "-" : names);
    }
    if (!logging)
        return;
    // Which of the follower's spells the AI did not list, and their
    // magicka at the moment, since a cost above the pool is the first guess
    // at the filter that kept a pinned Chain Lightning out (03:25).
    std::string missing;
    const auto consider = [&](RE::SpellItem *spell) {
        if (spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell && !listed.contains(spell))
            missing += (missing.empty() ? "" : ", ") + std::string(NameOr(spell, "?")) + " (" +
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
    log::pins.debug("{} combat inventory left out: {} -- magicka {:.0f}/{:.0f}", Describe(actor),
                    missing.empty() ? "nothing" : missing,
                    owner ? owner->GetActorValue(RE::ActorValue::kMagicka) : 0.0f,
                    owner ? owner->GetPermanentActorValue(RE::ActorValue::kMagicka) : 0.0f);
}

} // namespace

// Mark the scanned items and spells that are pinned, and those the AI is
// kept from, for the panel's cells; and drop any pin for something the
// scans did not find: sold, dropped, the last arrow shot. The watchdog
// catches that case on its own, but there is no reason to leave a dead pin
// for it to find.
void MarkPins(RE::Actor *actor, std::vector<InventoryItem> &items, std::vector<MagicEntry> &magic)
{
    std::scoped_lock lock(g_pinMutex);
    // A row is marked by a ban on its variant, or on every copy of its form.
    if (const auto bans = g_bans.find(actor->GetFormID()); bans != g_bans.end())
    {
        const auto bansRow = [&](std::uint32_t form, const std::optional<ft::ItemVariant> &variant) {
            return std::any_of(bans->second.begin(), bans->second.end(),
                               [&](const Banned &b) { return b.form == form && SameVariant(b.variant, variant); });
        };
        for (auto &item : items)
            item.banned = bansRow(item.form, item.variant);
        for (auto &entry : magic)
            entry.banned = bansRow(entry.form, {});
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
            const std::string name = NameOr(holder, "Something");
            lines += (lines.empty() ? "" : "\n") + std::string(name) + " is pinned";
        }
        return lines;
    };

    // The detail page's Equipped row is built by the scan, before the pins
    // are known; the pin glyph goes on it here.
    const auto pinGlyph = [](std::vector<SheetSection> &detail) {
        for (auto &section : detail)
            for (auto &row : section.rows)
                if (row.equipped)
                    row.icon2 = kGlyphPin;
    };

    // A pin marks the incumbent: the worn row of its variant, where the pin
    // says. A variant covers several rows (the clean stack and the poisoned
    // dagger), and the marker goes on the one in the hand, which is what
    // the follower is holding; with none worn -- the pin waiting on the
    // watchdog -- every row of the variant is marked until one is. A pin on
    // the form, whichever copy (a rule's), marks the same way over all
    // its rows. The pins themselves are pruned by the watchdog, which asks
    // the bag; the panel only marks.
    const auto wornWhere = [&](const InventoryItem *row, Hand hands) {
        if (!row)
            return true; // a spell's entry: the pin's own hands say it all
        return hands == Hand::None ? row->worn
                                   : (Overlap(hands, Hand::Left) && row->equippedLeft) ||
                                         (Overlap(hands, Hand::Right) && row->equippedRight);
    };
    const auto pinOfRow = [&](std::uint32_t form, const std::optional<ft::ItemVariant> &variant,
                              const InventoryItem *row) -> const Pin * {
        for (const Pin &pin : pins)
        {
            if (pin.thing.form != form || !SameVariant(pin.thing.variant, variant))
                continue;
            if (wornWhere(row, pin.hands))
                return &pin;
            // Not the incumbent: marked only while no row of the variant is.
            const bool anyWorn = std::any_of(items.begin(), items.end(), [&](const InventoryItem &other) {
                return other.form == form && SameVariant(pin.thing.variant, other.variant) &&
                       wornWhere(&other, pin.hands);
            });
            if (!anyWorn)
                return &pin;
        }
        return nullptr;
    };
    const auto mark = [&](std::uint32_t form, const std::optional<ft::ItemVariant> &variant, const InventoryItem *row,
                          bool &left, bool &right, bool &aside, std::string &asideBy, bool &whole,
                          std::vector<SheetSection> &detail) {
        if (const Pin *pin = pinOfRow(form, variant, row))
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
        const Holdable described = DescribeHoldable(actor, thing, variant);
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
                if ((pin.hands == Hand::Left || pin.hands == Hand::Right) && !SameThing(pin.thing, described) &&
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
        mark(item.form, item.variant, &item, item.pinnedLeft, item.pinnedRight, item.setAside, item.asideBy,
             item.pinned, item.detail);
    for (auto &entry : magic)
        mark(entry.form, {}, nullptr, entry.pinnedLeft, entry.pinnedRight, entry.setAside, entry.asideBy, entry.pinned,
             entry.detail);
}

// Put a spell in a hand -- Left, Right, or None for the engine's choice --
// unless it is there already. The engine's item equip is a no-op for an
// item already worn; its spell equip is not, and each call plays the equip
// sound, so the panel and the watchdog together could sound several times
// for one pin. Returns whether anything was done.
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

void KeepPins(const std::vector<RE::Actor *> &followers)
{
    EnforcePins(followers);
    for (auto *follower : followers)
        ProbeCombatInventory(follower);
}

namespace
{

// One request against the book, on the game thread: the panel's task and
// the rules' tick both come here. False when a pin is refused, so a rule's
// refused pin is not reported done.
bool Wear(RE::Actor *actor, RE::TESForm *thing, WearRequest request, Hand hand, bool fromPanel,
          const std::optional<ft::ItemVariant> &variant = std::nullopt, RE::ExtraDataList *row = nullptr)
{
    const ft::ActorId id = actor->GetFormID();
    const Holdable described = DescribeHoldable(actor, thing, variant);
    Hand hands = HandsFor(described.grip, hand);
    if (request == WearRequest::Pin && !Pinnable(described))
    {
        // Neither the panel nor the rules offer this; a pin is a promise the
        // AI would not keep, and it is refused here too rather than
        // half-kept as an equip without a pin.
        log::pins.warn("{} {} cannot be pinned: above the follower's skill, the AI would not choose it",
                       Describe(actor), log::NameOf(thing));
        return false;
    }
    // One weapon cannot be in both hands. Asked to move their only copy to
    // the other hand, take it out of the first; otherwise the engine's
    // equip, finding none free, conjures a second (02:05, the doubled
    // dagger). Two in the bag may go one per hand.
    bool moving = false;
    if ((request == WearRequest::Pin || request == WearRequest::Equip) && thing->Is(RE::FormType::Weapon) &&
        (hands == Hand::Left || hands == Hand::Right))
    {
        // Asked of the variant, count and hand both: the one tempered dagger
        // moves across though three plain ones stay in the bag, and the
        // tempered one clicked right while a plain one is in the left is not
        // a move at all.
        const Hand other = Without(Hand::Both, hands);
        auto *object = thing->As<RE::TESBoundObject>();
        if (object && Worn(actor, object, other, variant))
            moving = described.count < 2;
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
    const char *const by = fromPanel ? "player" : "rule";
    {
        std::scoped_lock lock(g_pinMutex);
        auto &pins = g_pins[id];
        // A rule's pin in a fight goes over what the follower had before it:
        // the pins remembered for after the fight, overridden while it lasts.
        const auto remembered = g_pinsBeforeFight.find(id);
        const std::vector<Pin> *before = !fromPanel && g_fighting.contains(id) && remembered != g_pinsBeforeFight.end()
                                             ? &remembered->second
                                             : nullptr;
        if (!fromPanel && request == WearRequest::Pin)
        {
            const auto bans = g_bans.find(id);
            if (bans != g_bans.end() && IsBanned(bans->second, described))
            {
                std::vector<log::Field> fields = ItemFields(described.form, variant, hands, by);
                fields.emplace_back("inCombat", g_fighting.contains(id));
                log::pins.event(log::Level::Info, "ban.overridden", actor, fields,
                                "{} a rule pins banned {} -- the pin holds while it lasts", Describe(actor),
                                log::NameOf(thing));
            }
        }
        // What the request does to the book is core's (ApplyRequest); the
        // bans are the game side's own list. The panel's word mid-fight is
        // the new normal: the same change goes into the book remembered for
        // after the fight, so the player's pin is what comes back, not the
        // one it replaced. A rule's pin is for the fight only and leaves the
        // remembered book alone.
        const auto bookRequest = request == WearRequest::Pin     ? PinRequest::Pin
                                 : request == WearRequest::Equip ? PinRequest::Equip
                                                                 : PinRequest::Ban;
        if (request != WearRequest::Unban && request != WearRequest::Unequip)
        {
            ApplyToBook(actor, pins, bookRequest, described, hands, moving, dualWield, by, before);
            if (fromPanel && g_fighting.contains(id)) [[maybe_unused]]
                const auto mirrored =
                    ApplyRequest(g_pinsBeforeFight[id], bookRequest, described, hands, moving, dualWield);
        }
        if (request == WearRequest::Ban)
            Ban(g_bans[id], described.form, described.variant);
        else if (request == WearRequest::Unban)
            Unban(g_bans[id], described.form, described.variant);
        g_refusedLogged.clear();
        // The book changed: a violation of what it holds now is news.
        ClearReports(g_pinsEnforced, id);
        ClearReports(g_bansEnforced, id);
    }

    const std::string name = NameOr(thing, "?");
    // Where the style forbids two, a one-hander into one hand takes the
    // one-hander out of the other, pinned (its pin went above) or merely
    // equipped: the engine's equip of this hand leaves the other as it
    // was, and the follower would stand with a weapon in each. The only
    // copy moving across is not this (WouldDualWield).
    if (!dualWield && (request == WearRequest::Pin || request == WearRequest::Equip) &&
        (hands == Hand::Left || hands == Hand::Right))
    {
        const Hand other = Without(Hand::Both, hands);
        if (auto *held = actor->GetEquippedObject(other == Hand::Left))
        {
            const Holdable inOther = DescribeHoldable(actor, held);
            if (WouldDualWield(described, &inOther))
            {
                log::pins.info("{} {} comes off the {} hand: the combat style does not dual wield", Describe(actor),
                               log::NameOf(held), other == Hand::Left ? "left" : "right");
                UnequipForm(actor, held, other, true);
            }
        }
    }
    switch (request)
    {
    case WearRequest::Equip:
        log::pins.event(log::Level::Info, "equip.applied", actor, ItemFields(described.form, variant, hands, by),
                        "{} told to ready {}{} (not pinned)", Describe(actor), name, HandTag(hands));
        if (moving)
            UnequipForm(actor, thing, Without(Hand::Both, hands), true, variant);
        EquipPinned(actor, thing, hands, true, variant, row, fromPanel);
        break;
    case WearRequest::Unequip:
        log::pins.info("{} told to put away {}{}", Describe(actor), name, HandTag(hands));
        UnequipForm(actor, thing, hands, true, variant);
        break;
    case WearRequest::Ban:
        log::pins.event(log::Level::Info, "ban.applied", actor, ItemFields(described.form, variant, Hand::None, by),
                        "{} told never to use {} (banned)", Describe(actor), name);
        TakeOffEverywhere(actor, thing, described, true);
        break;
    case WearRequest::Unban:
        log::pins.event(log::Level::Info, "ban.released", actor, ItemFields(described.form, variant, Hand::None, by),
                        "{} may use {} again (ban lifted)", Describe(actor), name);
        break;
    case WearRequest::Pin:
        log::pins.event(log::Level::Info, "pin.applied", actor, ItemFields(described.form, variant, hands, by),
                        "{} told to ready {}{} (pinned, by the {})", Describe(actor), name, HandTag(hands), by);
        if (moving)
            UnequipForm(actor, thing, Without(Hand::Both, hands), true, variant);
        EquipPinned(actor, thing, hands, true, variant, row, fromPanel);
        // Which hand a weapon or spell lands in is the AI's call as much
        // as ours: say what was asked and where it went, so the rule can
        // be read off the log.
        if (thing->Is(RE::FormType::Weapon))
        {
            log::pins.debug("{} weapon {} asked {} -- now left {} right {}", Describe(actor), name,
                            static_cast<int>(hands), actor->GetEquippedObject(true) == thing,
                            actor->GetEquippedObject(false) == thing);
        }
        if (auto *spell = thing->As<RE::SpellItem>())
        {
            const auto *slot = spell->GetEquipSlot();
            const auto &data = actor->GetActorRuntimeData();
            log::pins.debug("{} spell {} asked {} -- record slot {:06X} -- now left {} right {} -- {}", Describe(actor),
                            name, static_cast<int>(hands), slot ? slot->GetFormID() : 0,
                            data.selectedSpells[RE::Actor::SlotTypes::kLeftHand] == spell,
                            data.selectedSpells[RE::Actor::SlotTypes::kRightHand] == spell, CasterState(actor));
        }
        if (thing->Is(RE::FormType::Shout))
            log::pins.debug("{} shout {} -- {}", Describe(actor), name, CasterState(actor));
        break;
    }

    // Redraw them now. The Creation Kit wiki, on EquipItem: armour
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
    return true;
}

} // namespace

void RequestWear(ft::ActorId id, std::uint32_t form, WearRequest request, Hand hand,
                 std::optional<ft::ItemVariant> variant, RE::ExtraDataList *row)
{
    auto *task = SKSE::GetTaskInterface();
    if (!task)
        return;
    // Queued to the game thread and run there once. The panel is open while
    // this is clicked, and with FreezeTimeOnMenu the tick is held, so the
    // task also republishes their view: the cell answers now rather than when
    // the panel closes.
    task->AddTask([id, form, request, hand, variant = std::move(variant), row]() {
        auto *actor = RE::TESForm::LookupByID<RE::Actor>(id);
        auto *thing = RE::TESForm::LookupByID(form);
        if (!actor || !thing)
            return;
        Wear(actor, thing, request, hand, true, variant, row);
        // The page the panel is on, at once: the clock is frozen while it is
        // open, so the tick's own refresh is held and the cell would
        // otherwise answer only when the panel closed.
        RefreshShownPage();
        // A spell or a shout leaves a hand, or the voice, through a Papyrus
        // native that the VM runs a frame later, so the build above read the
        // actor before it had moved and the cell redrew as it was. This was
        // caught by the next beat's refresh until the beat stopped rebuilding
        // the page (2026-09-15); nothing came after it then, and with the
        // player's cell having nothing but the actor to read -- a follower's
        // also carries the pin and the ban, which change here and now -- an
        // unequip took two clicks, the second only redrawing what the first
        // had already done. The panel's next frame takes this instead.
        //
        // A spell going ON needs no second look, and nor does an item: both
        // are done by the time this returns. Asking for the two of them
        // rather than telling them apart down in UnequipForm keeps the reason
        // in one place, and the cost is one page build on a click the player
        // made, not on a beat.
        if (thing->Is(RE::FormType::Spell) || thing->Is(RE::FormType::Shout))
            RefreshShownPageSoon();
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
            entries.push_back({pin.thing.form, pin.thing.variant, pin.hands});
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
            log::pins.warn("{} saved pin {:08X} names nothing in this game -- forgotten", Describe(actor), entry.form);
            continue;
        }
        const std::string name = NameOr(thing, "?");
        const Holdable described = DescribeHoldable(actor, thing, entry.variant);
        auto *object = thing->As<RE::TESBoundObject>();
        const bool on =
            described.IsVoice() ? InVoice(actor, thing) : object && Worn(actor, object, entry.hands, entry.variant);
        if (!on || !Pinnable(described))
        {
            log::pins.warn("{} saved pin on {}{} ({}) does not hold -- {} -- forgotten", Describe(actor), name,
                           HandTag(entry.hands), ft::VariantText(entry.variant),
                           !on ? "not worn now" : "cannot be pinned");
            continue;
        }
        AddPin(book, described, entry.hands, false);
        log::pins.event(log::Level::Info, "pin.applied", actor,
                        ItemFields(entry.form, entry.variant, entry.hands, "save"), "{} saved pin on {}{} taken back",
                        Describe(actor), name, HandTag(entry.hands));
    }
}

void AdoptBans(RE::Actor *actor, const Bans &bans)
{
    if (!actor || bans.empty())
        return;
    std::scoped_lock lock(g_pinMutex);
    auto &book = g_bans[actor->GetFormID()];
    for (const Banned &ban : bans)
    {
        auto *thing = RE::TESForm::LookupByID(ban.form);
        if (!thing)
        {
            log::pins.warn("{} saved ban {:08X} names nothing in this game -- forgotten", Describe(actor), ban.form);
            continue;
        }
        // A ban on a variant holds only while a row of it is carried:
        // none, and it has nothing to promise about.
        auto *object = thing->As<RE::TESBoundObject>();
        const bool present = !ban.variant || (object && CountVariant(actor, object, ban.variant) > 0);
        if (!present)
        {
            log::pins.warn("{} saved ban on {} -- no longer carried -- forgotten", Describe(actor), log::NameOf(thing));
            continue;
        }
        if (Ban(book, ban.form, ban.variant))
            log::pins.event(log::Level::Info, "ban.applied", actor,
                            ItemFields(ban.form, ban.variant, Hand::None, "save"), "{} saved ban on {} taken back",
                            Describe(actor), log::NameOf(thing));
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
    g_pinsEnforced.clear();
    g_bansEnforced.clear();
}

bool PinNow(RE::Actor *actor, std::uint32_t form, Hand hand, const std::optional<ft::ItemVariant> &variant)
{
    auto *thing = RE::TESForm::LookupByID(form);
    if (!actor || !thing)
        return false;
    // A variant must have a row in the bag: the snapshot said so, and this is
    // the check for one that left between the snapshot and now.
    if (variant)
    {
        auto *object = thing->As<RE::TESBoundObject>();
        if (object && CountVariant(actor, object, variant) <= 0)
            return false;
    }
    // An either-hand thing asked for both hands is pinned once in each; the
    // book's AddPin joins the two. Everything else takes the hands its
    // record gives it, whatever was asked.
    if (hand == Hand::Both && DescribeHoldable(actor, thing).grip == Grip::Either)
    {
        const bool left = Wear(actor, thing, WearRequest::Pin, Hand::Left, false, variant);
        const bool right = Wear(actor, thing, WearRequest::Pin, Hand::Right, false, variant);
        return left && right;
    }
    return Wear(actor, thing, WearRequest::Pin, hand, false, variant);
}

void ReleaseKind(RE::Actor *actor, Kind kind, Hand hands)
{
    if (!actor)
        return;
    // The pins of that kind -- in those hands, when hands are named --
    // taken out of the book first so the watchdog and the score hook see
    // them gone, then taken off. A two-hander's pin holds both hands and
    // goes with either.
    std::vector<Pin> released;
    // Which of them the follower had before a fight this rule is in: the
    // player's, overridden for the fight rather than let go.
    std::vector<bool> overridden;
    {
        std::scoped_lock lock(g_pinMutex);
        const auto it = g_pins.find(actor->GetFormID());
        if (it == g_pins.end())
            return;
        std::erase_if(it->second, [&](const Pin &pin) {
            if (pin.thing.kind != kind || (hands != Hand::None && !Overlap(pin.hands, hands)))
                return false;
            released.push_back(pin);
            return true;
        });
        const auto remembered = g_pinsBeforeFight.find(actor->GetFormID());
        const bool fighting = g_fighting.contains(actor->GetFormID()) && remembered != g_pinsBeforeFight.end();
        for (const Pin &pin : released)
            overridden.push_back(fighting && FindPin(remembered->second, pin.thing.form, pin.thing.variant));
        g_refusedLogged.clear();
        ClearReports(g_pinsEnforced, actor->GetFormID());
    }
    for (std::size_t r = 0; r < released.size(); ++r)
    {
        const Pin &pin = released[r];
        auto *thing = RE::TESForm::LookupByID(pin.thing.form);
        if (!thing)
            continue;
        std::vector<log::Field> fields = ItemFields(pin.thing.form, pin.thing.variant, pin.hands, "rule");
        if (overridden[r])
        {
            fields.emplace_back("overriddenByFormId", log::Id(0));
            fields.emplace_back("overriddenByName", "nothing: the AI decides");
            log::pins.event(log::Level::Info, "pin.overridden", actor, fields,
                            "{} a rule lets go of {}{} for the fight -- the AI decides again", Describe(actor),
                            log::NameOf(thing), HandTag(pin.hands));
        }
        else
        {
            fields.emplace_back("reason", "a rule let go; the AI decides again");
            log::pins.event(log::Level::Info, "pin.released", actor, fields,
                            "{} told to let go of {}{} -- the AI decides again", Describe(actor), log::NameOf(thing),
                            HandTag(pin.hands));
        }
        UnequipForm(actor, thing, pin.hands, true, pin.thing.variant);
    }
    // And whatever of the kind is on, pinned or not: an unequip is an
    // unequip. The AI decides again from empty, as it does after the pins.
    // A weapon or a spell by the hands named; the arrows in the quiver;
    // every piece of armour worn.
    bool bared = false;
    const auto off = [&](RE::TESForm *thing, Hand hand) {
        if (!thing)
            return;
        UnequipForm(actor, thing, hand, true);
        bared = true;
    };
    switch (kind)
    {
    case Kind::Weapon:
    case Kind::Spell:
        for (const Hand hand : {Hand::Left, Hand::Right})
        {
            if (hands != Hand::None && !Overlap(hands, hand))
                continue;
            RE::TESForm *held = actor->GetEquippedObject(hand == Hand::Left);
            if (held && held->Is(RE::FormType::Spell) == (kind == Kind::Spell))
                off(held, hand);
        }
        break;
    case Kind::Ammo:
        off(actor->GetCurrentAmmo(), Hand::None);
        break;
    case Kind::Armor:
        if (auto *changes = actor->GetInventoryChanges(); changes && changes->entryList)
        {
            std::vector<RE::TESForm *> worn;
            for (auto *entry : *changes->entryList)
                if (entry && entry->object && entry->object->IsArmor() && entry->IsWorn())
                    worn.push_back(entry->object);
            for (RE::TESForm *piece : worn)
                off(piece, Hand::None);
        }
        break;
    default:
        break;
    }
    if (!released.empty() || bared)
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
using EquipSpellFn = void (*)(RE::ActorEquipManager *, RE::Actor *, RE::SpellItem *, const RE::BGSEquipSlot *);
using EquipShoutFn = void (*)(RE::ActorEquipManager *, RE::Actor *, RE::TESShout *);
EquipObjectFn g_equipObject = nullptr;
EquipSpellFn g_equipSpell = nullptr;
EquipShoutFn g_equipShout = nullptr;

// Would this equip, which is not ours, break a pin or a ban? The rule is
// core's (RefusesEngineEquip, tested against the simulated engine); a ban
// refuses outright. Two things pass: a bound weapon, the conjuration in
// progress, since refusing it ends the spell they are casting (Follower
// Equip Control found this the hard way) and the score hook keeps the AI
// from choosing the spell for a pinned hand anyway; and the spell or shout
// a record of ours is casting, whose equip is the package's on our behalf
// -- a rule may cast a banned spell, and a cast borrows a pinned hand.
// One function for an item, a spell and a shout: the AI, a script and a
// package all reach the same three engine entries, and a spell ban was a
// fight between a mod's script and the watchdog until the spell entry was
// detoured too (Megara's Heal Other, 2026-09-11).
// The bag as the engine's own equip sees it, for an equip naming no list:
// the copies in the entry's order, the listless remainder first. What
// EnginePick walks.
std::vector<VariantInBag> VariantsInBag(RE::Actor *actor, RE::TESBoundObject *object,
                                        std::vector<RE::ExtraDataList *> &lists)
{
    std::vector<VariantInBag> copies;
    lists.clear();
    const Carried carried = CarriedOf(actor, object);
    if (carried.count <= 0)
        return copies;
    std::int32_t distinct = 0;
    std::vector<VariantInBag> listed;
    if (carried.entry && carried.entry->extraLists)
    {
        for (auto *list : *carried.entry->extraLists)
        {
            if (!list)
                continue;
            const bool isDistinct = RowOfItsOwn(list);
            if (isDistinct)
                distinct += list->GetCount();
            listed.push_back({VariantOf(list), !isDistinct, ListWorn(list, Hand::None)});
            lists.push_back(list);
        }
    }
    if (carried.count > distinct)
    {
        copies.push_back({ItemVariant{}, true, false});
        lists.insert(lists.begin(), nullptr);
    }
    copies.insert(copies.end(), listed.begin(), listed.end());
    return copies;
}

// `extra` is the copy the engine reached for, or null for the form with
// the copy left to the engine -- the combat AI's equips, whose list is by
// form. Ours is only to gatekeep the pins and the bans. An equip naming a
// list is judged by that copy. One naming none is given the copy the
// engine would itself have reached with the banned copies left out of
// its pool (EnginePick, core, tested): the engine's own order, less the
// bans, and nothing chosen by us. When that pool is empty -- every copy
// banned, or every allowed one already worn -- the request goes on as it
// came, judged as the plain copy, and whatever the engine then does the
// watchdog answers.
bool Refused(RE::Actor *actor, RE::TESForm *form, RE::ExtraDataList *&extra, const RE::BGSEquipSlot *slot)
{
    // Out of the player's service the book waits, kept, and the engine
    // dresses them as it likes until they rejoin (Pins.h).
    if (!actor->IsPlayerTeammate() || IsOurCast(actor, form->GetFormID()))
        return false;
    std::scoped_lock lock(g_pinMutex);
    static const std::vector<Pin> kNoPins;
    static const Bans kNoBans;
    const auto pinsIt = g_pins.find(actor->GetFormID());
    const auto bansIt = g_bans.find(actor->GetFormID());
    const std::vector<Pin> &pins = pinsIt == g_pins.end() ? kNoPins : pinsIt->second;
    const Bans &bans = bansIt == g_bans.end() ? kNoBans : bansIt->second;
    if (pins.empty() && bans.empty())
        return false;
    // The copy the engine reached for, by the variant on its list; with no
    // list, the form, whichever variant. A spell or a shout has none.
    const bool equipment = form->IsWeapon() || form->IsArmor() || form->IsAmmo() || form->Is(RE::FormType::Light);
    auto *object = equipment ? form->As<RE::TESBoundObject>() : nullptr;
    Holdable thing = DescribeHoldable(actor, form, extra ? std::optional(VariantOf(extra)) : std::nullopt);
    const Hand into = SlotHand(slot);
    log::pins.debug("{} the engine equips {}{} ({})", Describe(actor), log::NameOf(form), HandTag(into),
                    CopyText(thing.variant));
    if (object && !pins.empty())
    {
        // The incumbent first: a copy of a pinned variant worn where this
        // equip is aimed. An equip that names no list, or names another copy
        // of the same variant, is a no-op to the pin's purpose, and is refused so
        // the engine's own pick cannot displace the incumbent with a plain
        // copy and set the watchdog flapping. The incumbent's own list is
        // the engine re-equipping what is there, and passes. The other
        // hand is not a no-op and goes on below.
        for (const Pin &pin : pins)
        {
            if (pin.thing.form != thing.form)
                continue;
            const bool here = pin.hands == Hand::None ? into == Hand::None : Overlap(pin.hands, into);
            if (!here)
                continue;
            // A pin on the form, whichever variant: any copy of it worn
            // there is the incumbent, and any other copy a no-op.
            RE::ExtraDataList *incumbent = pin.thing.variant
                                               ? WornVariantList(actor, object, *pin.thing.variant, pin.hands)
                                               : WornList(actor, object, pin.hands);
            if (!incumbent)
                continue;
            if (extra == incumbent)
                return false;
            if (!extra || SameVariant(VariantOf(extra), pin.thing.variant))
            {
                log::pins.debug("{} the engine equips {}{} over its pinned incumbent -- kept in place", Describe(actor),
                                log::NameOf(form), HandTag(into));
                return true;
            }
        }
    }
    if (!extra && object && !bans.empty())
    {
        std::vector<RE::ExtraDataList *> lists;
        const std::vector<VariantInBag> copies = VariantsInBag(actor, object, lists);
        if (const auto pick = EnginePick(copies, bans, thing.form))
        {
            // The engine's own first choice, a plain copy, needs no list
            // named: it reaches for one itself. A named copy goes by its
            // list.
            extra = lists[*pick];
            thing.variant = copies[*pick].variant;
            if (extra)
                log::pins.debug("{} the engine's pick for {}{} minus the bans: a row of another variant ({})",
                                Describe(actor), log::NameOf(form), HandTag(into), CopyText(thing.variant));
        }
    }
    const Refusal refusal = RefusesEngineEquip(pins, bans, thing, into, DualWieldAllowed(actor));
    if (!refusal)
        return false;
    if (refusal.why != Refusal::Why::Banned)
        if (const auto *weapon = form->As<RE::TESObjectWEAP>(); weapon && weapon->IsBound())
            return false;
    const std::string copy = CopyText(thing.variant);
    if (g_refusedLogged.insert(fmt::format("{:08X} {:08X} {}", actor->GetFormID(), form->GetFormID(), copy)).second)
    {
        if (refusal.why == Refusal::Why::Banned)
            log::pins.warn("{} the engine would equip banned {} ({}) -- refused ({})", Describe(actor),
                           log::NameOf(form), copy, actor->IsInCombat() ? "in combat" : "out of combat");
        else
        {
            const auto *held = RE::TESForm::LookupByID(refusal.pin->thing.form);
            const char *why = refusal.why == Refusal::Why::OneCopy    ? "one copy cannot fill both hands"
                              : refusal.why == Refusal::Why::OtherPin ? "another pin holds that hand"
                                                                      : "a pin holds the hand or slot";
            log::pins.warn("{} the engine would equip {}{} ({}) over pinned {}{} ({}) -- refused: {} ({})",
                           Describe(actor), log::NameOf(form), HandTag(HandsFor(thing.grip, into)), copy,
                           log::NameOf(held), HandTag(refusal.pin->hands), ft::VariantText(refusal.pin->thing.variant),
                           why, actor->IsInCombat() ? "in combat" : "out of combat");
        }
    }
    return true;
}

void EquipObjectHook(RE::ActorEquipManager *self, RE::Actor *actor, RE::TESBoundObject *object,
                     RE::ExtraDataList *extra, std::uint32_t count, const RE::BGSEquipSlot *slot, bool queue,
                     bool force, bool sounds, bool applyNow)
{
    if (g_ownEquipDepth == 0 && actor && object && Refused(actor, object, extra, slot))
        return;
    g_equipObject(self, actor, object, extra, count, slot, queue, force, sounds, applyNow);
}

void EquipSpellHook(RE::ActorEquipManager *self, RE::Actor *actor, RE::SpellItem *spell, const RE::BGSEquipSlot *slot)
{
    RE::ExtraDataList *none = nullptr;
    if (g_ownEquipDepth == 0 && actor && spell && Refused(actor, spell, none, slot))
        return;
    g_equipSpell(self, actor, spell, slot);
}

void EquipShoutHook(RE::ActorEquipManager *self, RE::Actor *actor, RE::TESShout *shout)
{
    RE::ExtraDataList *none = nullptr;
    if (g_ownEquipDepth == 0 && actor && shout && Refused(actor, shout, none, nullptr))
        return;
    g_equipShout(self, actor, shout);
}

// One detour, by address-library id: the same pair the library's own
// wrapper resolves. Says so at error level when it cannot be placed.
template <typename Fn> bool Detour(const char *what, REL::RelocationID id, Fn &original, Fn hook)
{
    const REL::Relocation<std::uintptr_t> target{id};
    original = reinterpret_cast<Fn>(target.address());
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID &>(original), reinterpret_cast<PVOID>(hook));
    const LONG result = DetourTransactionCommit();
    if (result != NO_ERROR)
    {
        log::pins.error("could not detour {} (Detours error {}) -- the engine's equips of that kind will not be "
                        "refused against the pins and bans",
                        what, result);
        return false;
    }
    log::pins.info("{} at {:X} detoured -- refused against the pins and bans", what, target.address());
    return true;
}

} // namespace

void RefuseEquipsAgainstPins()
{
    // The engine's three equip entries (SE / AE ids): an item, a spell into
    // a hand, a shout or power into the voice.
    Detour("ActorEquipManager::EquipObject", RELOCATION_ID(37938, 38894), g_equipObject, &EquipObjectHook);
    Detour("ActorEquipManager::EquipSpell", RELOCATION_ID(37939, 38895), g_equipSpell, &EquipSpellHook);
    Detour("ActorEquipManager::EquipShout", RELOCATION_ID(37941, 38897), g_equipShout, &EquipShoutHook);
}

} // namespace ft::game
