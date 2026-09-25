#include "game/Inventory.h"

#include "core/I18n.h"

#include "game/Sheet.h"

#include "game/Magic.h"

#include "game/Sensors.h"

#include "game/Bag.h"
#include "game/EffectRows.h"
#include "game/Log.h"
#include "game/Pins.h"
#include "game/Spells.h"
#include "game/Util.h"
#include "game/Values.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace ft::game
{
using ft::i18n::Tr;
using ft::i18n::TrFormat;

namespace
{

// Replace every `token` in `text`, ignoring case. The effect descriptions in
// Skyrim.esm write <mag>; mods are not so consistent, and the engine takes
// either.
void ReplaceNoCase(std::string &text, std::string_view token, const std::string &with)
{
    const auto same = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    auto at = std::search(text.begin(), text.end(), token.begin(), token.end(), same);
    while (at != text.end())
    {
        const auto index = static_cast<std::size_t>(at - text.begin());
        text.replace(index, token.size(), with);
        at = std::search(text.begin() + static_cast<std::ptrdiff_t>(index + with.size()), text.end(), token.begin(),
                         token.end(), same);
    }
}

// The record's own DESC text, which most items leave empty.
template <typename T> std::string DescriptionOf(T *item)
{
    RE::BSString text;
    item->GetDescription(text, item);
    return text.c_str() ? text.c_str() : "";
}

const char *WeaponTypeName(const RE::TESObjectWEAP *weapon)
{
    using Type = RE::WEAPON_TYPE;
    switch (weapon->GetWeaponType())
    {
    case Type::kOneHandSword:
        return Tr("Sword");
    case Type::kOneHandDagger:
        return Tr("Dagger");
    case Type::kOneHandAxe:
        return Tr("War Axe");
    case Type::kOneHandMace:
        return Tr("Mace");
    case Type::kTwoHandSword:
        return Tr("Greatsword");
    case Type::kTwoHandAxe:
        // The record does not distinguish them; the keyword does.
        return weapon->HasKeywordString("WeapTypeWarhammer") ? Tr("Warhammer") : Tr("Battleaxe");
    case Type::kBow:
        return Tr("Bow");
    case Type::kStaff:
        return Tr("Staff");
    case Type::kCrossbow:
        return Tr("Crossbow");
    default:
        return Tr("Weapon");
    }
}

// The charge of this row's copy: its ExtraCharge, full with none, and while
// it is held the hand's live value, which the engine writes back to the
// copy only on unequip (ChargeOf, 2026-09-08). The enchantment and the full
// charge are the form's.
WeaponCharge RowCharge(RE::Actor *actor, RE::TESObjectWEAP *weapon, RE::InventoryEntryData *entry)
{
    WeaponCharge c = ChargeOf(actor, weapon, Hand::None);
    if (!c.enchanted || !entry || !entry->extraLists)
        return c;
    c.charge = c.maxCharge;
    for (auto *list : *entry->extraLists)
    {
        if (!list)
            continue;
        if (const auto *xCharge = list->GetByType<RE::ExtraCharge>())
            c.charge = xCharge->charge;
        const bool left = list->HasType(RE::ExtraDataType::kWornLeft);
        if (auto *owner = actor->AsActorValueOwner(); owner && (left || list->HasType(RE::ExtraDataType::kWorn)))
            c.charge = owner->GetActorValue(left ? RE::ActorValue::kLeftItemCharge : RE::ActorValue::kRightItemCharge);
    }
    c.charge = std::clamp(c.charge, 0.0f, c.maxCharge);
    return c;
}

} // namespace

ft::Grip ArmorGrip(const RE::TESObjectARMO *armor)
{
    // The hand slots (Sensors.h) may sit above a slot of the mod's own, so
    // the parents are walked.
    const RE::BGSEquipSlot *root = armor->GetEquipSlot();
    if (!root)
        return ft::Grip::None;
    if (root->GetFormID() == kEitherHandSlot)
        return ft::Grip::Either;
    if (root->GetFormID() == kBothHandsSlot)
        return ft::Grip::Both;
    ft::Hand hands = ft::Hand::None;
    std::vector<const RE::BGSEquipSlot *> open{root};
    std::vector<const RE::BGSEquipSlot *> seen;
    while (!open.empty())
    {
        const RE::BGSEquipSlot *slot = open.back();
        open.pop_back();
        if (std::find(seen.begin(), seen.end(), slot) != seen.end())
            continue;
        seen.push_back(slot);
        if (slot->GetFormID() == kLeftHandSlot)
            hands = hands | ft::Hand::Left;
        else if (slot->GetFormID() == kRightHandSlot)
            hands = hands | ft::Hand::Right;
        for (const RE::BGSEquipSlot *parent : slot->parentSlots)
        {
            if (parent)
                open.push_back(parent);
        }
    }
    switch (hands)
    {
    case ft::Hand::Left:
        return ft::Grip::LeftOnly;
    case ft::Hand::Right:
        return ft::Grip::RightOnly;
    case ft::Hand::Both:
        return ft::Grip::Either;
    default:
        return ft::Grip::None;
    }
}

ft::Grip SpellGrip(const RE::SpellItem *spell)
{
    // By FormID: the four slots are fixed in Skyrim.esm, and the default
    // object table did not answer for them (01:29, "Either" for a left-hand
    // record).
    const auto *slot = spell->GetEquipSlot();
    const std::uint32_t slotId = slot ? slot->GetFormID() : 0;
    return spell->IsTwoHanded()       ? ft::Grip::Both
           : slotId == kLeftHandSlot  ? ft::Grip::LeftOnly
           : slotId == kRightHandSlot ? ft::Grip::RightOnly
                                      : ft::Grip::Either;
}

namespace
{

// "Heavy Helmet", "Light Boots", "Gloves", "Ring": the class and the piece,
// except for jewellery and shields, where the piece says enough.
std::string ArmorTypeName(const RE::TESObjectARMO *armor)
{
    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
    using Class = RE::BGSBipedObjectForm::ArmorType;

    if (armor->HasPartOf(Slot::kShield))
        return Tr("Shield");
    if (armor->HasPartOf(Slot::kRing))
        return Tr("Ring");
    if (armor->HasPartOf(Slot::kAmulet))
        return Tr("Amulet");
    if (armor->HasPartOf(Slot::kCirclet))
        return Tr("Circlet");

    const Class armorClass = armor->GetArmorType();
    const bool clothing = armorClass == Class::kClothing;
    const char *piece = Tr("Armor");
    if (armor->HasPartOf(Slot::kBody))
        piece = clothing ? Tr("Clothes") : Tr("Armor");
    else if (armor->HasPartOf(Slot::kHead) || armor->HasPartOf(Slot::kHair))
        piece = clothing ? Tr("Hat") : Tr("Helmet");
    else if (armor->HasPartOf(Slot::kHands))
        piece = clothing ? Tr("Gloves") : Tr("Gauntlets");
    else if (armor->HasPartOf(Slot::kFeet))
        piece = clothing ? Tr("Shoes") : Tr("Boots");
    else if (clothing)
        piece = Tr("Clothing");

    if (clothing)
        return piece;
    return armorClass == Class::kHeavyArmor ? TrFormat("Heavy {}", piece) : TrFormat("Light {}", piece);
}

std::string SkillName(RE::ActorValue skill)
{
    auto *list = RE::ActorValueList::GetSingleton();
    auto *info = list ? list->GetActorValueInfo(skill) : nullptr;
    return info ? NameOf(info) : "?";
}

// What the container menu would leave out. Armour and weapons carry their
// own non-playable flag, and the engine's GetPlayable honours it for both;
// for every other kind the base record flag is not a reliable answer, so
// only the name is checked. The record's own name, not the copy's display
// name: the engine dresses a nameless record as "<Missing Name>", and the
// Unarmed weapon (Skyrim.esm 0x1F4), not flagged non-playable, was listed
// under it once a script put it in the player's bag (2026-09-13).
bool IsListed(RE::TESBoundObject *object)
{
    if (NameOf(object).empty())
        return false;
    if (object->Is(RE::FormType::Armor) || object->Is(RE::FormType::Weapon))
        return object->GetPlayable();
    return true;
}

// The type word, the category, and the type-specific rows and prose.
void Classify(RE::Actor *actor, RE::TESBoundObject *object, RE::InventoryEntryData *entry, InventoryItem &item,
              SheetSection &stats)
{
    item.type = Tr("Item");
    item.category = ItemCategory::Misc;

    if (auto *weapon = object->As<RE::TESObjectWEAP>())
    {
        item.type = WeaponTypeName(weapon);
        item.category = ItemCategory::Weapons;
        item.equipable = true;
        item.handItem = true;
        item.grip = DescribeHoldable(actor, weapon).grip;
        // In their hands, as the inventory menu would show it; the record's
        // own figure is the first line of its hover.
        {
            SheetRow row;
            item.damage = WeaponDamage(actor, weapon, entry, &row.breakdown);
            row.label = Tr("Damage");
            row.value = Fmt("%.0f", item.damage);
            stats.rows.push_back(std::move(row));
        }
        stats.rows.push_back(Row(Tr("Critical Damage"), std::to_string(weapon->GetCritDamage())));
        {
            SheetRow row;
            const float chance = CritChance(actor, weapon, &row.breakdown);
            row.label = Tr("Critical Chance");
            row.value = Fmt("%.0f%%", chance);
            stats.rows.push_back(std::move(row));
        }
        {
            // The left hand's multiplier for a copy worn there alone, the
            // right hand's otherwise: the hand a carried weapon swings in
            // is not known until it is drawn.
            bool wornLeft = false;
            bool wornRight = false;
            if (entry && entry->extraLists)
            {
                for (auto *list : *entry->extraLists)
                {
                    wornLeft = wornLeft || (list && list->HasType(RE::ExtraDataType::kWornLeft));
                    wornRight = wornRight || (list && list->HasType(RE::ExtraDataType::kWorn));
                }
            }
            SheetRow row;
            const float speed = WeaponSpeed(actor, weapon, wornLeft && !wornRight, &row.breakdown);
            row.label = Tr("Speed");
            row.value = Fmt("%.2f", speed);
            stats.rows.push_back(std::move(row));
        }
        stats.rows.push_back(Row(Tr("Reach"), Fmt("%.2f", weapon->GetReach())));
        stats.rows.push_back(Row(Tr("Stagger"), Fmt("%.2f", weapon->GetStagger())));
        item.description = DescriptionOf(weapon);
        return;
    }
    if (auto *armor = object->As<RE::TESObjectARMO>())
    {
        item.type = ArmorTypeName(armor);
        item.category = ItemCategory::Armor;
        item.equipable = true;
        // Armour that takes a hand -- a shield, or a mod's hand-held piece
        // -- lists with the weapons, as ammunition does: it is chosen with
        // the sword, and a shield bashes. That leaves armour with no hand
        // and a single Equipped column.
        item.grip = ArmorGrip(armor);
        if (item.grip != ft::Grip::None)
        {
            item.category = ItemCategory::Weapons;
            item.handItem = true;
            item.leftOnly = item.grip == ft::Grip::LeftOnly;
            item.rightOnly = item.grip == ft::Grip::RightOnly;
        }
        if (armor->GetArmorType() != RE::BGSBipedObjectForm::ArmorType::kClothing)
        {
            SheetRow row;
            item.armor = ArmorRating(actor, armor, entry, &row.breakdown);
            row.label = Tr("Armor");
            row.value = Fmt("%.0f", item.armor);
            stats.rows.push_back(std::move(row));
        }
        item.description = DescriptionOf(armor);
        return;
    }
    if (auto *ammo = object->As<RE::TESAmmo>())
    {
        item.type = ammo->IsBolt() ? Tr("Bolt") : Tr("Arrow");
        item.category = ItemCategory::Arrows;
        item.equipable = true;
        item.damage = ammo->GetRuntimeData().data.damage;
        stats.rows.push_back(Row(Tr("Damage"), Fmt("%.0f", item.damage)));
        return;
    }
    // The first effect's name, for the list: what the thing is for. The
    // rest are on its page. (An ingredient gives its first when eaten and
    // no other, so for one this is the whole truth.)
    const auto effectName = [](const RE::MagicItem *magic) -> std::string {
        for (const auto *effect : ResolvedEffects(*magic))
        {
            const char *name = effect->baseEffect->GetFullName();
            if (name && *name)
                return name;
        }
        return "";
    };
    if (auto *alch = object->As<RE::AlchemyItem>())
    {
        item.effect = effectName(alch);
        if (alch->IsPoison())
        {
            item.type = Tr("Poison");
            item.category = ItemCategory::Poisons;
        }
        else if (alch->IsFood())
        {
            item.type = Tr("Food");
            item.category = ItemCategory::Food;
        }
        else
        {
            item.type = Tr("Potion");
            item.category = ItemCategory::Potions;
        }
        item.effectsTable = EffectsOf(actor, alch, [](const RE::Effect *e) { return e->effectItem.magnitude; });
        return;
    }
    if (auto *ingredient = object->As<RE::IngredientItem>())
    {
        item.type = Tr("Ingredient");
        item.category = ItemCategory::Ingredients;
        item.effect = effectName(ingredient);
        item.effectsTable = EffectsOf(actor, ingredient, [](const RE::Effect *e) { return e->effectItem.magnitude; });
        return;
    }
    if (auto *scroll = object->As<RE::ScrollItem>())
    {
        // A scroll is a spell in a wrapper, read once: its page and its
        // list carry what a spell's do -- school, type, magnitude,
        // duration, charge time, cast -- less the level and the cost, which
        // a scroll has not.
        item.type = Tr("Scroll");
        item.category = ItemCategory::Scrolls;
        item.effect = effectName(scroll);
        item.effectsTable = EffectsOf(actor, scroll, [](const RE::Effect *e) { return e->effectItem.magnitude; });
        item.cast = CastWord(scroll->GetDelivery(), scroll->GetCastingType());
        const auto *costliest = scroll->GetCostliestEffectItem();
        const auto *effect = costliest ? costliest->baseEffect : nullptr;
        if (effect)
        {
            const auto school = SchoolOf(effect->GetMagickSkill());
            if (school != MagicCategory::COUNT)
                stats.rows.push_back(Row(Tr("School"), DisplayName(school)));
            if (const std::string kind = TypeWord(effect); !kind.empty())
                stats.rows.push_back(Row(Tr("Kind"), kind)); // the spell page's Type; Type here says Scroll
            item.magnitude = ActualMagnitude(actor, scroll, costliest);
            stats.rows.push_back(Row(Tr("Magnitude"), Fmt("%.0f", item.magnitude)));
            if (const float duration = ActualDuration(actor, scroll, costliest); duration > 0.0f)
                stats.rows.push_back(Row(Tr("Duration"), TrFormat("{} s", Fmt("%.0f", duration))));
        }
        if (const float charge = scroll->GetChargeTime(); charge > 0.0f)
            stats.rows.push_back(Row(Tr("Charge Time"), TrFormat("{} s", Fmt("%.1f", charge))));
        stats.rows.push_back(Row(Tr("Cast"), item.cast));
        return;
    }
    if (auto *book = object->As<RE::TESObjectBOOK>())
    {
        item.category = ItemCategory::Books;
        if (book->TeachesSpell())
        {
            item.type = Tr("Spell Tome");
            item.spellTome = true;
            RE::SpellItem *spell = book->GetSpell();
            stats.rows.push_back(Row(Tr("Teaches"), NameOf(spell)));
            if (IsCastable(spell))
            {
                item.teaches = spell->GetFormID();
                item.teachesName = NameOf(spell);
                item.knowsTaught = actor->HasSpell(spell);
            }
        }
        else
        {
            item.type = Tr("Book");
            if (book->TeachesSkill())
                stats.rows.push_back(Row(Tr("Teaches"), SkillName(book->GetSkill())));
        }
        // The card text, not the book's own -- that runs to pages.
        RE::BSString text;
        book->itemCardDescription.GetDescription(text, book);
        item.description = text.c_str() ? text.c_str() : "";
        return;
    }
    if (auto *gem = object->As<RE::TESSoulGem>())
    {
        item.type = Tr("Soul Gem");
        stats.rows.push_back(Row(Tr("Capacity"), SoulName(gem->GetMaximumCapacity())));
        // The soul in this particular gem lives on the entry, not the record:
        // a filled Grand gem is the same base object as an empty one.
        const RE::SOUL_LEVEL soul = entry ? entry->GetSoulLevel() : gem->GetContainedSoul();
        stats.rows.push_back(Row(Tr("Contains"), SoulName(soul)));
        item.filledSoulGem = soul != RE::SOUL_LEVEL::kNone;
        return;
    }
    if (object->Is(RE::FormType::KeyMaster))
    {
        item.type = Tr("Key");
        item.category = ItemCategory::Keys;
        return;
    }
    if (object->Is(RE::FormType::Light))
    {
        // With the weapons for the same reason as a shield: it takes a hand.
        item.type = Tr("Torch");
        item.category = ItemCategory::Weapons;
        item.equipable = true;
        item.handItem = true;
        item.leftOnly = true;
        item.grip = ft::Grip::LeftOnly;
        return;
    }
    if (object->Is(RE::FormType::Misc))
        item.type = Tr("Misc");
}

} // namespace

namespace
{
} // namespace

float ActualMagnitude(RE::Actor *caster, RE::MagicItem *spell, const RE::Effect *effect, RE::Actor *target)
{
    float value = effect ? effect->effectItem.magnitude : 0.0f;
    if (caster && spell)
        RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSpellMagnitude, caster, spell, target,
                                            &value);
    return value;
}

std::string DescriptionFor(RE::Actor *caster, RE::MagicItem *spell, RE::TESDescription &description)
{
    RE::BSString text;
    description.GetDescription(text, nullptr);
    std::string out = text.c_str() ? text.c_str() : "";
    if (out.empty())
        return out;
    const auto *costliest = spell ? spell->GetCostliestEffectItem() : nullptr;
    if (costliest)
    {
        ReplaceNoCase(out, "<mag>", Fmt("%.0f", ActualMagnitude(caster, spell, costliest)));
        ReplaceNoCase(out, "<dur>", Fmt("%.0f", ActualDuration(caster, spell, costliest)));
        ReplaceNoCase(out, "<area>", std::to_string(costliest->effectItem.area));
    }
    return out;
}

float ActualDuration(RE::Actor *caster, RE::MagicItem *spell, const RE::Effect *effect)
{
    float value = effect ? static_cast<float>(effect->effectItem.duration) : 0.0f;
    if (caster && spell)
        RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSpellDuration, caster, spell,
                                            static_cast<RE::Actor *>(nullptr), &value);
    return value;
}

const char *DisplayName(ItemCategory category)
{
    switch (category)
    {
    case ItemCategory::Weapons:
        return Tr("Weapons");
    case ItemCategory::Arrows:
        return Tr("Arrows");
    case ItemCategory::Armor:
        return Tr("Armor");
    case ItemCategory::Potions:
        return Tr("Potions");
    case ItemCategory::Poisons:
        return Tr("Poisons");
    case ItemCategory::Food:
        return Tr("Food");
    case ItemCategory::Ingredients:
        return Tr("Ingredients");
    case ItemCategory::Scrolls:
        return Tr("Scrolls");
    case ItemCategory::Books:
        return Tr("Books");
    case ItemCategory::Keys:
        return Tr("Keys");
    case ItemCategory::Misc:
    default:
        return Tr("Misc");
    }
}

namespace
{

// One row: `count` copies of `object` described by `entry`, which holds
// only this row's extra lists; `variant` is the row's, plain for the plain
// stack.
void DescribeStack(RE::Actor *actor, RE::TESBoundObject *object, RE::InventoryEntryData *entry, std::int32_t count,
                   std::uint32_t stack, const ft::ItemVariant &variant, std::vector<InventoryItem> &out)
{
    InventoryItem item;
    item.form = object->GetFormID();
    item.stack = stack;
    item.variant = variant;
    item.name = entry && entry->GetDisplayName() ? entry->GetDisplayName() : NameOf(object);
    if (!IsListed(object))
        return;

    item.count = static_cast<int>(count);
    // -1 is the engine's "this kind has no weight record" -- ammunition
    // in Special Edition -- and it means weightless, not a debt.
    item.weight = (std::max)(0.0f, object->GetWeight());
    // The engine's own figure: enchantment and soul included, as the
    // trade menu prices it.
    item.value = entry ? entry->GetValue() : object->GetGoldValue();
    item.worn = entry && entry->IsWorn();
    // Stolen when a copy on the row is, by the engine's own ownership rule
    // asked from the player's side, not by who the owner is: a gift carries
    // the player's ownership and is nobody's theft. The engine stacks owners,
    // so one row can hold stolen and honest copies, and marks the row.
    if (auto *player = RE::PlayerCharacter::GetSingleton(); player && entry && entry->extraLists)
    {
        for (auto *list : *entry->extraLists)
        {
            auto *owner = list ? list->GetOwner() : nullptr;
            item.stolen = item.stolen || (owner && !entry->IsOwnedBy(player, owner, true));
        }
    }
    if (auto *keyworded = object->As<RE::BGSKeywordForm>())
    {
        static auto *artifact = RE::TESForm::LookupByID<RE::BGSKeyword>(0x000A8668);
        static auto *vendor = RE::TESForm::LookupByID<RE::BGSKeyword>(0x000917E8);
        item.artifact = (artifact && keyworded->HasKeyword(artifact)) || (vendor && keyworded->HasKeyword(vendor));
    }
    SheetSection stats{Tr("Stats"), {}, {}};
    {
        // The FormID first, as the spell page has it: what the console
        // and the log call the thing.
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", object->GetFormID());
        stats.rows.push_back(Row(Tr("Base ID"), id));
    }
    // The Type row's index: after the id.
    const std::size_t typeRow = stats.rows.size();
    stats.rows.push_back(Row(Tr("Type"), ""));
    Classify(actor, object, entry, item, stats);
    // In hand, by this row's own lists: with the plain dagger in one hand
    // and the tempered one in the other, the form is in both hands and
    // each row is in one. A worn copy always has a list, with the mark for
    // the hand it is in; a shield or a torch carries the one Worn mark and
    // is the left hand's by its kind (WornIn).
    if (item.handItem && entry && entry->extraLists)
    {
        // A two-hander's worn copy carries the right hand's mark alone and
        // holds both hands, as the engine reports it: the left cell read
        // empty beside a crossbow once the ticks came from the marks.
        const bool twoHanded = TwoHanded(object->As<RE::TESObjectWEAP>());
        for (const auto *list : *entry->extraLists)
        {
            const bool right = WornIn(object, list, Hand::Right);
            item.equippedLeft = item.equippedLeft || WornIn(object, list, Hand::Left) || (twoHanded && right);
            item.equippedRight = item.equippedRight || right;
        }
    }
    stats.rows[typeRow].value = item.type;
    if (item.count > 1)
    {
        stats.rows.push_back(Row(Tr("Count"), std::to_string(item.count)));
        stats.rows.push_back(Row(Tr("Weight"), TrFormat("{} each, {} in all", Fmt("%.1f", item.weight),
                                                        Fmt("%.1f", item.weight * static_cast<float>(item.count)))));
        stats.rows.push_back(Row(Tr("Value"), TrFormat("{} each, {} in all", item.value, item.value * item.count)));
    }
    else
    {
        stats.rows.push_back(Row(Tr("Weight"), Fmt("%.1f", item.weight)));
        stats.rows.push_back(Row(Tr("Value"), std::to_string(item.value)));
    }
    // An outfit piece: added by the actor's Outfit record when they
    // loaded and marked on its entry, which is what the trade menu hides
    // it by. A tick when it is; no row when it is not.
    if (entry && entry->extraLists)
    {
        bool outfit = false;
        for (auto *list : *entry->extraLists)
            outfit = outfit || (list && list->GetByType<RE::ExtraOutfitItem>() != nullptr);
        if (outfit)
        {
            SheetRow row;
            row.label = Tr("Outfit");
            row.icon = kGlyphTick;
            stats.rows.push_back(std::move(row));
        }
    }
    if (item.worn)
    {
        // The pin glyph beside the tick is added by MarkPins, which runs
        // after this scan and is the one that knows the pins.
        SheetRow equipped;
        equipped.label = Tr("Equipped");
        equipped.icon = kGlyphTick;
        stats.rows.push_back(std::move(equipped));
    }
    item.detail.push_back(std::move(stats));

    // A poison on a weapon: a dose on one of the entry's extra lists,
    // hits rather than seconds, with the poison's own record behind it.
    // The same two shapes as an enchantment: one headed row, and the
    // effects' table.
    if (item.category == ItemCategory::Weapons && entry && entry->extraLists)
    {
        for (auto *list : *entry->extraLists)
        {
            auto *dose = list ? list->GetByType<RE::ExtraPoison>() : nullptr;
            if (!dose || !dose->poison)
                continue;
            item.poison = SheetSection{Tr("Poison"), {Row(NameOf(dose->poison), std::to_string(dose->count))}, {}};
            item.poisonEffects =
                EffectsOf(actor, dose->poison, [](const RE::Effect *e) { return e->effectItem.magnitude; });
            item.poisonEffects.title = Tr("Poison Effects");
            break;
        }
    }

    // An enchantment, whether the record's or one put on at an arcane
    // enchanter: the entry answers for both.
    if (RE::EnchantmentItem *ench = entry ? entry->GetEnchantment() : nullptr)
    {
        item.enchanted = true;
        // One row: the name, and what is left of the charge over the
        // full amount, as numbers and as the share: "89 / 100 (89%)". A
        // weapon never used has no ExtraCharge and is full.
        const std::string name = NameOf(ench);
        std::string charge;
        if (auto *weapon = object->As<RE::TESObjectWEAP>())
        {
            const WeaponCharge c = RowCharge(actor, weapon, entry);
            item.chargeable = c.enchanted && c.maxCharge > 0.0f;
            item.charge = c.charge;
            item.maxCharge = c.maxCharge;
            if (c.enchanted && c.maxCharge > 0.0f)
            {
                char text[64];
                std::snprintf(text, sizeof(text), "%.0f / %.0f (%.0f%%)", static_cast<double>(c.charge),
                              static_cast<double>(c.maxCharge), static_cast<double>(100.0f * c.charge / c.maxCharge));
                charge = text;
            }
        }
        else if (const auto left = entry->GetEnchantmentCharge())
            charge = Fmt("%.0f%%", *left);
        item.enchantment = SheetSection{Tr("Enchantment"), {Row(name.empty() ? Tr("(unnamed)") : name, charge)}, {}};
        item.effectsTable = EffectsOf(actor, ench, [](const RE::Effect *e) { return e->effectItem.magnitude; });
        // Named for the enchantment, as a poison's are for the poison:
        // a bare "Effects" under an "Enchantment" heading read as a
        // second thing.
        item.effectsTable.title = Tr("Enchantment Effects");
    }

    out.push_back(std::move(item));
}

} // namespace

std::vector<InventoryItem> ScanInventory(RE::Actor *actor)
{
    std::vector<InventoryItem> out;
    if (!actor)
        return out;

    // The whole bag, not just the potions: this is the one scan whose job is
    // to show everything.
    auto inventory = actor->GetInventory();
    for (auto &[object, slot] : inventory)
    {
        const auto count = slot.first;
        RE::InventoryEntryData *entry = slot.second.get();
        if (!object || count <= 0)
            continue;
        // A leveled list an NPC's record put in the bag (Marcurio's
        // LItemWeaponDaggerBest): the engine resolved it to a real dagger
        // when he loaded, and the list itself stays behind, nameless. Not a
        // thing.
        if (object->Is(RE::FormType::LeveledItem))
            continue;

        // The bag keeps one entry per form, and the entry's accessors (the
        // name, the value, the enchantment, worn) answer from whichever of
        // its extra lists they meet first, which mixes the copies: Frea's
        // outfit Nordic Carved Armor and the enchanted one they were given
        // read as one row of two, the plain one's rating and the enchanted
        // one's price (2026-09-11). So the copies that stand apart are each
        // described from an entry of their own holding just their list --
        // what the game's menu does per stack -- and the rest from one
        // holding the remainder. A temporary entry owns only its list
        // container; the lists themselves stay the bag's.
        // Which rows the tab shows is core's (core/BagView.h, DisplayRows),
        // over the same view the equips read: one algorithm for what a row
        // is, not one here and one there.
        const Bag bag = ViewOf(object, static_cast<std::int32_t>(count), entry);
        RE::InventoryEntryData plain(object, 0);
        std::uint32_t stack = 0;
        for (std::size_t i = 0; i < bag.view.rows.size(); ++i)
        {
            const ft::BagRow &row = bag.view.rows[i];
            // Which entries a list carries and whether that kept it apart,
            // said once per shape per bag at debug, so a row that reads
            // wrong against the game's menu has its list in the log.
            static std::unordered_set<std::string> seen;
            const std::string shape = ListEntries(bag.lists[i]);
            if (seen.insert(
                        fmt::format("{:08X}:{:08X}:{}:{}", actor->GetFormID(), object->GetFormID(), shape, row.ownRow))
                    .second)
                log::sensors.debug("{} {} list [{}] x{}: {}", Describe(actor), NameOf(object), shape, row.count,
                                   row.ownRow ? "a row of its own" : "folded into the stack");
            if (!row.ownRow)
                plain.AddExtraList(bag.lists[i]);
        }
        for (const ft::DisplayRow &drawn : ft::DisplayRows(bag.view))
        {
            if (!drawn.row)
            {
                plain.countDelta = drawn.count;
                DescribeStack(actor, object, &plain, drawn.count, 0, ft::ItemVariant{}, out);
                continue;
            }
            RE::ExtraDataList *list = bag.lists[*drawn.row];
            RE::InventoryEntryData one(object, drawn.count);
            one.AddExtraList(list);
            const std::size_t before = out.size();
            DescribeStack(actor, object, &one, drawn.count, ++stack, drawn.variant, out);
            if (out.size() > before)
                out.back().row = list;
        }
    }

    // Ties by key, or two rows of one name -- the plain stack and the
    // enchanted copy -- would swap places from one scan to the next.
    std::sort(out.begin(), out.end(), [](const InventoryItem &a, const InventoryItem &b) {
        const int byName = a.name.compare(b.name);
        return byName != 0 ? byName < 0 : a.Key() < b.Key();
    });
    return out;
}

} // namespace ft::game
