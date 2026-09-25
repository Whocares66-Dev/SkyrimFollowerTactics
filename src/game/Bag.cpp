#include "game/Bag.h"

#include "game/Sensors.h"

#include "game/Sheet.h"

#include "core/Blows.h"
#include "core/CustomSkills.h"
#include "core/Effects.h"
#include "core/I18n.h"
#include "core/Party.h"
#include "core/Reach.h"
#include "core/Spells.h"
#include "core/Vocabulary.h"

#include "game/CustomSkillsFramework.h"
#include "game/Hits.h"
#include "game/Inventory.h"
#include "game/Log.h"
#include "game/Magic.h"
#include "game/Packages.h"
#include "game/Pins.h"
#include "game/Settings.h"
#include "game/Toggles.h"
#include "game/Util.h"
#include "progression/game/Service.h"
#include "progression/game/ValueView.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ft::game
{

Carried CarriedOf(RE::Actor *actor, RE::TESBoundObject *object)
{
    Carried out;
    if (!actor || !object)
        return out;
    auto inventory = actor->GetInventory([object](RE::TESBoundObject &c) { return &c == object; });
    const auto found = inventory.find(object);
    if (found == inventory.end())
        return out;
    out.count = found->second.first;
    out.entry = std::move(found->second.second);
    return out;
}

bool TwoHanded(const RE::TESObjectWEAP *weapon)
{
    return weapon &&
           (weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() || weapon->IsCrossbow());
}
namespace
{

// The first extra list of the actor's entry for the object that `pick`
// accepts. A player's enchantment, tempering, poison and charge live on
// the INSTANCE's list, not the record, and each copy worn has its own.
RE::ExtraDataList *ListOf(RE::Actor *actor, RE::TESBoundObject *object, auto pick)
{
    auto *changes = actor && object ? actor->GetInventoryChanges() : nullptr;
    if (!changes || !changes->entryList)
        return nullptr;
    for (auto *entry : *changes->entryList)
    {
        if (!entry || entry->object != object)
            continue;
        if (!entry->extraLists)
            return nullptr;
        for (auto *list : *entry->extraLists)
        {
            if (list && pick(*list))
                return list;
        }
        return nullptr;
    }
    return nullptr;
}

} // namespace

RE::ExtraDataList *UnwornList(RE::Actor *actor, RE::TESBoundObject *object)
{
    return ListOf(actor, object, [](const RE::ExtraDataList &list) {
        return !list.HasType(RE::ExtraDataType::kWorn) && !list.HasType(RE::ExtraDataType::kWornLeft);
    });
}

RE::ExtraDataList *WornList(RE::Actor *actor, RE::TESBoundObject *object, Hand hand)
{
    return ListOf(actor, object, [object, hand](const RE::ExtraDataList &list) { return WornIn(object, &list, hand); });
}

ft::ItemVariant VariantOf(const RE::ExtraDataList *list)
{
    ft::ItemVariant variant;
    if (!list)
        return variant;
    using T = RE::ExtraDataType;
    // The enchantment as its recipe, not its form: one made at the table is
    // a form the save mints, found again by these same fields
    // (Effect::IsMatch), and no plugin names it.
    if (const auto *ench = static_cast<const RE::ExtraEnchantment *>(list->GetByType(T::kEnchantment));
        ench && ench->enchantment)
    {
        for (const RE::Effect *effect : ResolvedEffects(*ench->enchantment))
            variant.enchantment.push_back({effect->baseEffect->GetFormID(), effect->effectItem.magnitude,
                                           effect->effectItem.duration, effect->effectItem.area});
    }
    if (const auto *health = static_cast<const RE::ExtraHealth *>(list->GetByType(T::kHealth)); health)
        variant.tempering = health->health;
    // Only a variant the player gave: the engine adds a text entry of its own
    // to a tempered item the first time it draws it, with no text in it.
    if (const auto *text = static_cast<const RE::ExtraTextDisplayData *>(list->GetByType(T::kTextDisplayData));
        text && text->IsPlayerSet() && text->displayName.c_str())
        variant.label = text->displayName.c_str();
    return variant;
}
namespace
{

// The list's marks and count as a row, for the questions asked of one
// list: what the core's ListWorn and WornIn read.
ft::BagRow MarksOf(const RE::ExtraDataList *list)
{
    ft::BagRow row;
    row.wornRight = list->HasType(RE::ExtraDataType::kWorn);
    row.wornLeft = list->HasType(RE::ExtraDataType::kWornLeft);
    row.count = list->GetCount();
    row.token = list;
    return row;
}

} // namespace

Bag ViewBag(RE::Actor *actor, RE::TESBoundObject *object)
{
    const Carried carried = CarriedOf(actor, object);
    return ViewOf(object, carried.count, carried.entry.get());
}

Bag ViewOf(RE::TESBoundObject *object, std::int32_t count, const RE::InventoryEntryData *entry)
{
    Bag bag;
    bag.view.weapon = object && object->IsWeapon();
    if (count <= 0)
        return bag;
    bag.view.total = count;
    if (entry && entry->extraLists)
    {
        for (auto *list : *entry->extraLists)
        {
            if (!list)
                continue;
            ft::BagRow row = MarksOf(list);
            row.variant = VariantOf(list);
            row.ownRow = RowOfItsOwn(list);
            bag.view.rows.push_back(std::move(row));
            bag.lists.push_back(list);
        }
    }
    return bag;
}

RE::ExtraDataList *Bag::ListOf(const ft::BagRow *row) const noexcept
{
    return row ? lists[view.IndexOf(row)] : nullptr;
}

RE::ExtraDataList *Bag::ListAt(std::optional<std::size_t> index) const noexcept
{
    return index && *index < lists.size() ? lists[*index] : nullptr;
}

std::int32_t CountVariant(RE::Actor *actor, RE::TESBoundObject *object, const std::optional<ft::ItemVariant> &variant)
{
    return ft::CountVariant(ViewBag(actor, object).view, variant);
}

RE::ExtraDataList *WornVariantList(RE::Actor *actor, RE::TESBoundObject *object, const ft::ItemVariant &variant,
                                   Hand hands)
{
    const Bag bag = ViewBag(actor, object);
    return bag.ListOf(ft::WornVariantRow(bag.view, variant, hands));
}

RE::ExtraDataList *UnwornVariantList(RE::Actor *actor, RE::TESBoundObject *object, const ft::ItemVariant &variant)
{
    const Bag bag = ViewBag(actor, object);
    return bag.ListOf(ft::UnwornVariantRow(bag.view, variant));
}

RE::ExtraDataList *WornStackList(RE::Actor *actor, RE::TESBoundObject *object, Hand hands)
{
    const Bag bag = ViewBag(actor, object);
    return bag.ListOf(ft::WornStackRow(bag.view, hands));
}

RE::ExtraDataList *UnwornStackList(RE::Actor *actor, RE::TESBoundObject *object)
{
    const Bag bag = ViewBag(actor, object);
    return bag.ListOf(ft::UnwornStackRow(bag.view));
}

bool RowOfItsOwn(const RE::ExtraDataList *list)
{
    return list && !list->IsInventoryStackable(true);
}

RE::ExtraDataList *ListOfAddress(RE::Actor *actor, RE::TESBoundObject *object, const RE::ExtraDataList *address)
{
    if (!address)
        return nullptr;
    return ListOf(actor, object, [address](const RE::ExtraDataList &list) { return &list == address; });
}

std::vector<ft::ItemVariant> RowsOf(RE::Actor *actor, RE::TESBoundObject *object)
{
    return ft::RowsOf(ViewBag(actor, object).view);
}

bool HasListlessCopy(RE::Actor *actor, RE::TESBoundObject *object)
{
    return ViewBag(actor, object).view.HasListlessCopy();
}

std::string ListEntries(const RE::ExtraDataList *list)
{
    if (!list)
        return "-";
    std::string out;
    for (const auto &extra : *list)
    {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%s%02X", out.empty() ? "" : " ", static_cast<unsigned>(extra.GetType()));
        out += buf;
    }
    return out.empty() ? "empty" : out;
}

bool ListWorn(const RE::ExtraDataList *list, Hand hands)
{
    return list && ft::ListWorn(MarksOf(list), hands);
}

bool WornIn(const RE::TESBoundObject *object, const RE::ExtraDataList *list, Hand hands)
{
    if (!list)
        return false;
    ft::BagView kind;
    kind.weapon = !object || object->IsWeapon();
    return ft::WornIn(kind, MarksOf(list), hands);
}

RE::TESObjectWEAP *PoisonableWeaponIn(RE::Actor *actor, bool left)
{
    auto *weapon = WeaponIn(actor, left);
    // What the inventory menu offers a poison to: any weapon but a staff
    // (read from its poisoning routine, which checks that one type).
    if (!weapon || weapon->GetWeaponType() == RE::WEAPON_TYPE::kStaff)
        return nullptr;
    return weapon;
}

WeaponInHand WeaponToPoison(RE::Actor *actor)
{
    auto *right = PoisonableWeaponIn(actor, false);
    auto *left = PoisonableWeaponIn(actor, true);
    const Hand hand = ft::HandToPoison(right != nullptr, right && WeaponPoisoned(actor, right, Hand::Right),
                                       left != nullptr, left && WeaponPoisoned(actor, left, Hand::Left));
    if (hand == Hand::Right)
        return {right, hand};
    if (hand == Hand::Left)
        return {left, hand};
    return {};
}

bool WeaponPoisoned(RE::Actor *actor, RE::TESObjectWEAP *weapon, Hand hand)
{
    // The poison sits on the worn copy's extra list: that hand's, so two
    // of one dagger with one dosed read as one poisoned and one clean.
    // (The engine's own IsPoisoned reads every list of the entry, and
    // answered yes for both.)
    const RE::ExtraDataList *worn = WornList(actor, weapon, hand);
    return worn && worn->HasType(RE::ExtraDataType::kPoison);
}

RE::TESObjectWEAP *WeaponIn(RE::Actor *actor, bool left)
{
    if (!actor)
        return nullptr;
    auto *object = actor->GetEquippedObject(left);
    auto *weapon = object ? object->As<RE::TESObjectWEAP>() : nullptr;
    // Unarmed is a weapon record too and never in a bag. A two-hander or a
    // bow reports from the right hand and the left reports it again; a
    // one-hander in each hand is the same record twice, and both are
    // there, so two-handedness is what is asked, not sameness.
    if (!weapon || weapon->GetWeaponType() == RE::WEAPON_TYPE::kHandToHandMelee)
        return nullptr;
    if (left && TwoHanded(weapon))
        return nullptr;
    return weapon;
}

WeaponCharge ChargeOf(RE::Actor *actor, RE::TESObjectWEAP *weapon, Hand hand)
{
    WeaponCharge out;
    if (!actor || !weapon)
        return out;

    // The record's enchantment and full charge, or a player-made one's on
    // the copy's list (ExtraEnchantment carries both). What is left is
    // ExtraCharge, absent for a weapon never used. The same reading as
    // the engine's recharge routine. In a hand, that hand's copy; in the
    // bag, whichever lists the entry has.
    RE::EnchantmentItem *ench = weapon->formEnchanting;
    float max = static_cast<float>(weapon->amountofEnchantment);
    float charge = max;
    const auto read = [&](const RE::ExtraDataList *list) {
        if (!list)
            return;
        if (auto *xEnch = list->GetByType<RE::ExtraEnchantment>(); xEnch && xEnch->enchantment)
        {
            ench = xEnch->enchantment;
            max = static_cast<float>(xEnch->charge);
            charge = max;
        }
        if (auto *xCharge = list->GetByType<RE::ExtraCharge>())
            charge = xCharge->charge;
    };
    if (hand != Hand::None)
        read(WornList(actor, weapon, hand));
    else if (const Carried carried = CarriedOf(actor, weapon); carried.entry && carried.entry->extraLists)
    {
        for (auto *list : *carried.entry->extraLists)
            read(list);
    }
    if (!ench || max <= 0.0f)
        return out;
    // In hand, the live charge is an ACTOR VALUE -- RightItemCharge or
    // LeftItemCharge, what the HUD's charge meter reads -- and the item's
    // own record is only written back on unequip. Measured 2026-09-08: a
    // staff cast down to 491 showed no charge record until it was swapped
    // hands, and then 491 appeared. So the hand it is in says where to
    // read; in the bag, the record. Asked with no hand, a copy found worn
    // reads from the hand it is in, the right first.
    Hand in = hand;
    if (in == Hand::None)
        in = actor->GetEquippedObject(false) == weapon  ? Hand::Right
             : actor->GetEquippedObject(true) == weapon ? Hand::Left
                                                        : Hand::None;
    if (auto *owner = actor->AsActorValueOwner(); owner && in != Hand::None)
        charge = owner->GetActorValue(in == Hand::Right ? RE::ActorValue::kRightItemCharge
                                                        : RE::ActorValue::kLeftItemCharge);
    out.enchanted = true;
    out.charge = (std::min)((std::max)(charge, 0.0f), max);
    out.maxCharge = max;
    out.costPerHit = ench->CalculateMagickaCost(actor);

    return out;
}

float SoulCharge(RE::SOUL_LEVEL level)
{
    // The fallbacks are the executable's own defaults, read from its static
    // data: Skyrim.esm carries no record for these settings.
    switch (level)
    {
    case RE::SOUL_LEVEL::kPetty:
        return static_cast<float>(GameSetting("iSoulLevelValuePetty", 250));
    case RE::SOUL_LEVEL::kLesser:
        return static_cast<float>(GameSetting("iSoulLevelValueLesser", 500));
    case RE::SOUL_LEVEL::kCommon:
        return static_cast<float>(GameSetting("iSoulLevelValueCommon", 1000));
    case RE::SOUL_LEVEL::kGreater:
        return static_cast<float>(GameSetting("iSoulLevelValueGreater", 2000));
    case RE::SOUL_LEVEL::kGrand:
        return static_cast<float>(GameSetting("iSoulLevelValueGrand", 3000));
    default:
        return 0.0f;
    }
}

std::vector<ft::Snapshot::SoulGemView> ScanSoulGems(RE::Actor *actor)
{
    std::vector<ft::Snapshot::SoulGemView> out;
    if (!actor)
        return out;
    auto inventory = actor->GetInventory([](RE::TESBoundObject &obj) { return obj.Is(RE::FormType::SoulGem); });
    for (auto &[object, entry] : inventory)
    {
        const auto count = entry.first;
        if (count <= 0 || !object || !entry.second)
            continue;
        const float charge = SoulCharge(entry.second->GetSoulLevel());
        if (charge <= 0.0f)
            continue;
        out.push_back({object->GetFormID(), static_cast<int>(count), charge});
    }
    return out;
}

} // namespace ft::game
