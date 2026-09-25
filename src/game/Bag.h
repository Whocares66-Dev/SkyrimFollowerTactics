#pragma once
// The bag: what an actor carries, asked of the engine's inventory -- how
// many of a thing, its copies by variant, which copy is worn in which
// hand, a weapon's poison and charge, the soul gems.

#include "core/BagView.h"
#include "core/Blows.h"
#include "core/Breakdown.h"
#include "core/Effects.h"
#include "core/Rule.h"
#include "core/Snapshot.h"
#include "core/Views.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RE
{
class Actor;
class AlchemyItem;
class BGSAttackData;
struct Effect;
class InventoryEntryData;
class MagicItem;
class SpellItem;
class TESObjectARMO;
class TESObjectWEAP;
} // namespace RE

namespace ft::game
{

// One thing the actor carries, as the engine's inventory map reports it:
// how many, and the entry -- a copy, as GetInventory makes, with its
// extra lists -- or null for none carried. The twelve hand-written walks
// of that map were one lookup each.
struct Carried
{
    std::int32_t count{0};
    std::unique_ptr<RE::InventoryEntryData> entry;
};
[[nodiscard]] Carried CarriedOf(RE::Actor *actor, RE::TESBoundObject *object);

// One form's copies in the bag, read once: the core's view of them
// (core/BagView.h) with each row's list beside it, so a row the core
// chooses maps back to the list the engine is handed. Every question
// below that used to walk the lists asks the view.
struct Bag
{
    ft::BagView view;
    std::vector<RE::ExtraDataList *> lists; // one per view.rows, in order
    // The list of a row the core chose; null for none.
    [[nodiscard]] RE::ExtraDataList *ListOf(const ft::BagRow *row) const noexcept;
    [[nodiscard]] RE::ExtraDataList *ListAt(std::optional<std::size_t> index) const noexcept;
};
[[nodiscard]] Bag ViewBag(RE::Actor *actor, RE::TESBoundObject *object);
// The same from an entry already in hand -- the whole-bag scan walks the
// inventory once and has each entry.
[[nodiscard]] Bag ViewOf(RE::TESBoundObject *object, std::int32_t count, const RE::InventoryEntryData *entry);

// The hands are asked one at a time, and answered per COPY: with the same
// dagger in each hand -- two entries of one record -- the left's poison and
// charge are the left's, read off the extra list worn in that hand, not
// the right's read twice. A two-hander sits in the right hand and the left
// reports it again; asked about the left, these say nothing is there.
[[nodiscard]] bool TwoHanded(const RE::TESObjectWEAP *weapon);

// The extra list of the copy worn in that hand -- Left, Right, or either
// for a thing with no hand -- or the one worn nowhere, for an equip into
// the other hand. Null where there is none: an item with no extra data,
// which the engine takes as the plain case.
[[nodiscard]] RE::ExtraDataList *WornList(RE::Actor *actor, RE::TESBoundObject *object, Hand hand);
[[nodiscard]] RE::ExtraDataList *UnwornList(RE::Actor *actor, RE::TESBoundObject *object);
// Is the copy on this list worn in those hands: the right and armour are
// Worn, the left is WornLeft, None asks for either. False for no list.
[[nodiscard]] bool ListWorn(const RE::ExtraDataList *list, Hand hands);
// The same asked of a copy of this object: the worn marks by hand are a
// weapon's. A shield or a torch in the left hand carries the one Worn
// mark, as every piece of armour does, so for anything but a weapon the
// left hand or no hand is any mark, and the right hand is none.
[[nodiscard]] bool WornIn(const RE::TESBoundObject *object, const RE::ExtraDataList *list, Hand hands);
// Is the copy on this list a row of its own on the Inventory tab, rather
// than one of the plain stack: the engine's own answer, IsInventoryStackable
// (11598), which InventoryChanges::GetInventoryItemAt asks of each list as
// it numbers an inventory's items, and which the engine's equip asks before
// it reaches for "a plain copy". Its table, read from the running game
// 2026-09-13: tempering, a charge, a poison, a custom name, an enchantment
// and a soul keep a copy apart; ownership, a unique id, a reference handle,
// a scale, a torch's time left, the outfit and alias marks, the count and
// the hotkey fold it into the stack; worn marks are ignored. A poisoned
// dagger is its own row and rejoins the stack when the dose is gone. False
// for no list.
[[nodiscard]] bool RowOfItsOwn(const RE::ExtraDataList *list);
// The bag's list at this address, or null: a token from an earlier scan
// made safe to use, since the copy may have left and the address be
// another's or nobody's.
[[nodiscard]] RE::ExtraDataList *ListOfAddress(RE::Actor *actor, RE::TESBoundObject *object,
                                               const RE::ExtraDataList *address);
// The entries on a list as their type numbers, "16 3E" (ExtraDataType,
// hex), for a log line about which copy is which; "-" for no list.
[[nodiscard]] std::string ListEntries(const RE::ExtraDataList *list);
// The variant of the copy on this list (dev/UNIQUE.md "The variant"): its
// enchantment, tempering and custom name. Plain for a list with none of
// them, and for no list.
[[nodiscard]] ft::ItemVariant VariantOf(const RE::ExtraDataList *list);

// How many copies of the variant the bag holds: its rows summed, the
// listless remainder counting as plain; of the form, with no variant. What
// the one-copy rule asks.
[[nodiscard]] std::int32_t CountVariant(RE::Actor *actor, RE::TESBoundObject *object,
                                        const std::optional<ft::ItemVariant> &variant);
// A list of the variant, worn in those hands (None: worn at all) or not
// worn at all; null for none. Of the unworn, one of the plain stack before
// a row of its own: the poisoned dagger is the plain variant, but a click
// on the plain stack or a pin on it means a clean one while any is there.
// The plain variant may have no list to give: a listless copy is the
// engine's to resolve from a null list.
[[nodiscard]] RE::ExtraDataList *WornVariantList(RE::Actor *actor, RE::TESBoundObject *object,
                                                 const ft::ItemVariant &variant, Hand hands);
[[nodiscard]] RE::ExtraDataList *UnwornVariantList(RE::Actor *actor, RE::TESBoundObject *object,
                                                   const ft::ItemVariant &variant);
// A list of the plain stack -- one that is not a row of its own -- worn in
// those hands (None: worn at all), or not worn at all; null for none. The
// panel's click on the stack means one of these, or a listless copy.
[[nodiscard]] RE::ExtraDataList *WornStackList(RE::Actor *actor, RE::TESBoundObject *object, Hand hands);
[[nodiscard]] RE::ExtraDataList *UnwornStackList(RE::Actor *actor, RE::TESBoundObject *object);
// The form's rows in the bag as the Inventory tab splits them, by variant:
// one per list that is a row of its own, and the plain stack once when any
// copy is in it. What the core's whole-form questions take.
[[nodiscard]] std::vector<ft::ItemVariant> RowsOf(RE::Actor *actor, RE::TESBoundObject *object);
// Are there plain copies on no list at all, which only a null list can
// reach?
[[nodiscard]] bool HasListlessCopy(RE::Actor *actor, RE::TESBoundObject *object);

// The weapon in a hand, if it takes a poison (anything but a staff). Null
// for no weapon there, or a staff.
[[nodiscard]] RE::TESObjectWEAP *PoisonableWeaponIn(RE::Actor *actor, bool left);

// The weapon a poison would go on, and which hand it is in: the right
// hand's if it takes one and is clean, else the left's on the same terms,
// as the inventory menu goes to the right hand alone. Null when neither
// qualifies.
struct WeaponInHand
{
    RE::TESObjectWEAP *weapon{nullptr};
    Hand hand{Hand::None};
};
[[nodiscard]] WeaponInHand WeaponToPoison(RE::Actor *actor);

// Does the copy of that weapon worn in that hand have a poison on it?
[[nodiscard]] bool WeaponPoisoned(RE::Actor *actor, RE::TESObjectWEAP *weapon, Hand hand);

// A weapon's enchantment charge as the actor carries it: what is left, the
// full amount, and what one hit draws in the actor's hands. Not enchanted
// reads as all zero. `hand` names the worn copy to read -- and the hand
// whose live charge actor value holds what is left -- or None for a copy
// in the bag, read off its record and whatever list it has.
struct WeaponCharge
{
    bool enchanted{false};
    float charge{0.0f};
    float maxCharge{0.0f};
    float costPerHit{0.0f};
};
[[nodiscard]] WeaponCharge ChargeOf(RE::Actor *actor, RE::TESObjectWEAP *weapon, Hand hand);

// The weapon in a hand, enchanted or not; null for no weapon there.
[[nodiscard]] RE::TESObjectWEAP *WeaponIn(RE::Actor *actor, bool left);

// What a soul of that level puts into a charge: the five iSoulLevelValue
// game settings, which the engine's own recharge reads.
[[nodiscard]] float SoulCharge(RE::SOUL_LEVEL level);

// The filled soul gems carried, as the snapshot lists them. A reusable one
// (Azura's Star, the ReusableSoulGem keyword) counts: spending it empties
// it, as the engine's own recharge does, rather than removing it.
[[nodiscard]] std::vector<ft::Snapshot::SoulGemView> ScanSoulGems(RE::Actor *actor);
} // namespace ft::game
