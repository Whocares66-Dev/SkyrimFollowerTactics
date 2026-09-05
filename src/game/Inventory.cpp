#include "game/Inventory.h"

#include "game/Pins.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>

namespace ft::game
{
namespace
{

std::string Fmt(const char *fmt, double value)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), fmt, value);
    return buf;
}

SheetRow Row(std::string label, std::string value)
{
    SheetRow row;
    row.label = std::move(label);
    row.value = std::move(value);
    return row;
}

std::string NameOf(const RE::TESForm *form)
{
    return form && form->GetName() ? form->GetName() : "";
}

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
        return "Sword";
    case Type::kOneHandDagger:
        return "Dagger";
    case Type::kOneHandAxe:
        return "War Axe";
    case Type::kOneHandMace:
        return "Mace";
    case Type::kTwoHandSword:
        return "Greatsword";
    case Type::kTwoHandAxe:
        // The record does not distinguish them; the keyword does.
        return weapon->HasKeywordString("WeapTypeWarhammer") ? "Warhammer" : "Battleaxe";
    case Type::kBow:
        return "Bow";
    case Type::kStaff:
        return "Staff";
    case Type::kCrossbow:
        return "Crossbow";
    default:
        return "Weapon";
    }
}

} // namespace

ft::Grip ArmorGrip(const RE::TESObjectARMO *armor)
{
    // The slot records, by FormID from Skyrim.esm: RightHand 013F42,
    // LeftHand 013F43, EitherHand 013F44, BothHands 013F45. The first two
    // may sit above a slot of the mod's own, so the parents are walked.
    const RE::BGSEquipSlot *root = armor->GetEquipSlot();
    if (!root)
        return ft::Grip::None;
    if (root->GetFormID() == 0x00013F44)
        return ft::Grip::Either;
    if (root->GetFormID() == 0x00013F45)
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
        if (slot->GetFormID() == 0x00013F43)
            hands = hands | ft::Hand::Left;
        else if (slot->GetFormID() == 0x00013F42)
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

namespace
{

// "Heavy Helmet", "Light Boots", "Gloves", "Ring": the class and the piece,
// except for jewellery and shields, where the piece says enough.
std::string ArmorTypeName(const RE::TESObjectARMO *armor)
{
    using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
    using Class = RE::BGSBipedObjectForm::ArmorType;

    if (armor->HasPartOf(Slot::kShield))
        return "Shield";
    if (armor->HasPartOf(Slot::kRing))
        return "Ring";
    if (armor->HasPartOf(Slot::kAmulet))
        return "Amulet";
    if (armor->HasPartOf(Slot::kCirclet))
        return "Circlet";

    const Class armorClass = armor->GetArmorType();
    const bool clothing = armorClass == Class::kClothing;
    const char *piece = "Armor";
    if (armor->HasPartOf(Slot::kBody))
        piece = clothing ? "Clothes" : "Armor";
    else if (armor->HasPartOf(Slot::kHead) || armor->HasPartOf(Slot::kHair))
        piece = clothing ? "Hat" : "Helmet";
    else if (armor->HasPartOf(Slot::kHands))
        piece = clothing ? "Gloves" : "Gauntlets";
    else if (armor->HasPartOf(Slot::kFeet))
        piece = clothing ? "Shoes" : "Boots";
    else if (clothing)
        piece = "Clothing";

    if (clothing)
        return piece;
    return std::string(armorClass == Class::kHeavyArmor ? "Heavy " : "Light ") + piece;
}

const char *SoulName(RE::SOUL_LEVEL level)
{
    switch (level)
    {
    case RE::SOUL_LEVEL::kPetty:
        return "Petty";
    case RE::SOUL_LEVEL::kLesser:
        return "Lesser";
    case RE::SOUL_LEVEL::kCommon:
        return "Common";
    case RE::SOUL_LEVEL::kGreater:
        return "Greater";
    case RE::SOUL_LEVEL::kGrand:
        return "Grand";
    default:
        return "empty";
    }
}

std::string SkillName(RE::ActorValue skill)
{
    auto *list = RE::ActorValueList::GetSingleton();
    auto *info = list ? list->GetActorValue(skill) : nullptr;
    return info ? NameOf(info) : "?";
}

// What the container menu would leave out. Armour and weapons carry their
// own non-playable flag, and the engine's GetPlayable honours it for both;
// for every other kind the base record flag is not a reliable answer, so
// only the name is checked.
bool IsListed(RE::TESBoundObject *object, const std::string &name)
{
    if (name.empty())
        return false;
    if (object->Is(RE::FormType::Armor) || object->Is(RE::FormType::Weapon))
        return object->GetPlayable();
    return true;
}

// The type word, the category, and the type-specific rows and prose.
void Classify(RE::Actor *actor, RE::TESBoundObject *object, RE::InventoryEntryData *entry, InventoryItem &item,
              SheetSection &stats)
{
    item.type = "Item";
    item.category = ItemCategory::Misc;

    if (auto *weapon = object->As<RE::TESObjectWEAP>())
    {
        item.type = WeaponTypeName(weapon);
        item.category = ItemCategory::Weapons;
        item.equipable = true;
        item.handItem = true;
        item.grip = DescribeHoldable(actor, weapon).grip;
        // In her hands, as the inventory menu would show it; the record's
        // own figure beneath it, for the curious.
        item.damage = WeaponDamage(actor, weapon, entry);
        stats.rows.push_back(Row("Damage", Fmt("%.0f", item.damage)));
        stats.rows.push_back(Row("Base Damage", Fmt("%.0f", weapon->GetAttackDamage())));
        stats.rows.push_back(Row("Critical Damage", std::to_string(weapon->GetCritDamage())));
        stats.rows.push_back(Row("Speed", Fmt("%.2f", weapon->GetSpeed())));
        stats.rows.push_back(Row("Reach", Fmt("%.2f", weapon->GetReach())));
        stats.rows.push_back(Row("Stagger", Fmt("%.2f", weapon->GetStagger())));
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
            item.armor = ArmorRating(actor, armor, entry);
            stats.rows.push_back(Row("Armor", Fmt("%.0f", item.armor)));
            stats.rows.push_back(Row("Base Armor", Fmt("%.0f", armor->GetArmorRating())));
        }
        item.description = DescriptionOf(armor);
        return;
    }
    if (auto *ammo = object->As<RE::TESAmmo>())
    {
        item.type = ammo->IsBolt() ? "Bolt" : "Arrow";
        item.category = ItemCategory::Arrows;
        item.equipable = true;
        item.damage = ammo->GetRuntimeData().data.damage;
        stats.rows.push_back(Row("Damage", Fmt("%.0f", item.damage)));
        return;
    }
    if (auto *alch = object->As<RE::AlchemyItem>())
    {
        if (alch->IsPoison())
        {
            item.type = "Poison";
            item.category = ItemCategory::Potions;
        }
        else if (alch->IsFood())
        {
            item.type = "Food";
            item.category = ItemCategory::Food;
        }
        else
        {
            item.type = "Potion";
            item.category = ItemCategory::Potions;
        }
        item.effects = EffectLines(alch);
        return;
    }
    if (auto *ingredient = object->As<RE::IngredientItem>())
    {
        item.type = "Ingredient";
        item.category = ItemCategory::Ingredients;
        item.effects = EffectLines(ingredient);
        return;
    }
    if (auto *scroll = object->As<RE::ScrollItem>())
    {
        item.type = "Scroll";
        item.category = ItemCategory::Scrolls;
        item.effects = EffectLines(scroll);
        return;
    }
    if (auto *book = object->As<RE::TESObjectBOOK>())
    {
        item.category = ItemCategory::Books;
        if (book->TeachesSpell())
        {
            item.type = "Spell Tome";
            stats.rows.push_back(Row("Teaches", NameOf(book->GetSpell())));
        }
        else
        {
            item.type = "Book";
            if (book->TeachesSkill())
                stats.rows.push_back(Row("Teaches", SkillName(book->GetSkill())));
        }
        // The card text, not the book's own -- that runs to pages.
        RE::BSString text;
        book->itemCardDescription.GetDescription(text, book);
        item.description = text.c_str() ? text.c_str() : "";
        return;
    }
    if (auto *gem = object->As<RE::TESSoulGem>())
    {
        item.type = "Soul Gem";
        stats.rows.push_back(Row("Capacity", SoulName(gem->GetMaximumCapacity())));
        // The soul in this particular gem lives on the entry, not the record:
        // a filled Grand gem is the same base object as an empty one.
        stats.rows.push_back(Row("Contains", SoulName(entry ? entry->GetSoulLevel() : gem->GetContainedSoul())));
        return;
    }
    if (object->Is(RE::FormType::KeyMaster))
    {
        item.type = "Key";
        item.category = ItemCategory::Keys;
        return;
    }
    if (object->Is(RE::FormType::Light))
    {
        // With the weapons for the same reason as a shield: it takes a hand.
        item.type = "Torch";
        item.category = ItemCategory::Weapons;
        item.equipable = true;
        item.handItem = true;
        item.leftOnly = true;
        item.grip = ft::Grip::LeftOnly;
        return;
    }
    if (object->Is(RE::FormType::Misc))
        item.type = "Misc";
}

} // namespace

namespace
{
// One line per effect: the effect's own description with its numbers put
// in, or its name. `magnitude` and `duration` say what numbers.
std::string EffectLinesWith(const RE::MagicItem *magic, const std::function<float(const RE::Effect *)> &magnitude,
                            const std::function<float(const RE::Effect *)> &duration)
{
    std::string out;
    if (!magic)
        return out;
    for (const auto *effect : magic->effects)
    {
        if (!effect || !effect->baseEffect)
            continue;
        const char *text = effect->baseEffect->magicItemDescription.c_str();
        std::string line = text && *text ? text : NameOf(effect->baseEffect);
        if (line.empty())
            continue;
        ReplaceNoCase(line, "<mag>", Fmt("%.0f", magnitude(effect)));
        ReplaceNoCase(line, "<dur>", Fmt("%.0f", duration(effect)));
        ReplaceNoCase(line, "<area>", std::to_string(effect->effectItem.area));
        if (!out.empty())
            out += '\n';
        out += line;
    }
    return out;
}
} // namespace

std::string EffectLines(const RE::MagicItem *magic)
{
    return EffectLinesWith(
        magic, [](const RE::Effect *e) { return e->effectItem.magnitude; },
        [](const RE::Effect *e) { return static_cast<float>(e->effectItem.duration); });
}

std::string EffectLines(RE::Actor *caster, RE::MagicItem *spell)
{
    return EffectLinesWith(
        spell, [&](const RE::Effect *e) { return ActualMagnitude(caster, spell, e); },
        [&](const RE::Effect *e) { return ActualDuration(caster, spell, e); });
}

float ActualMagnitude(RE::Actor *caster, RE::MagicItem *spell, const RE::Effect *effect)
{
    float value = effect ? effect->effectItem.magnitude : 0.0f;
    if (caster && spell)
        RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSpellMagnitude, caster, spell,
                                            static_cast<RE::Actor *>(nullptr), &value);
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
        return "Weapons";
    case ItemCategory::Arrows:
        return "Arrows";
    case ItemCategory::Armor:
        return "Armor";
    case ItemCategory::Potions:
        return "Potions";
    case ItemCategory::Food:
        return "Food";
    case ItemCategory::Ingredients:
        return "Ingredients";
    case ItemCategory::Scrolls:
        return "Scrolls";
    case ItemCategory::Books:
        return "Books";
    case ItemCategory::Keys:
        return "Keys";
    case ItemCategory::Misc:
    default:
        return "Misc";
    }
}

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

        InventoryItem item;
        item.form = object->GetFormID();
        item.name = entry && entry->GetDisplayName() ? entry->GetDisplayName() : NameOf(object);
        if (!IsListed(object, item.name))
            continue;

        item.count = static_cast<int>(count);
        // -1 is the engine's "this kind has no weight record" -- ammunition
        // in Special Edition -- and it means weightless, not a debt.
        item.weight = (std::max)(0.0f, object->GetWeight());
        // The engine's own figure: enchantment and soul included, as the
        // trade menu prices it.
        item.value = entry ? entry->GetValue() : object->GetGoldValue();
        item.worn = entry && entry->IsWorn();
        item.equippedLeft = actor->GetEquippedObject(true) == object;
        item.equippedRight = actor->GetEquippedObject(false) == object;

        SheetSection stats{"Stats", {}, {}};
        {
            // The FormID first, as the spell page has it: what the console
            // and the log call the thing.
            char id[16];
            std::snprintf(id, sizeof(id), "%08X", object->GetFormID());
            stats.rows.push_back(Row("Base ID", id));
        }
        stats.rows.push_back(Row("Type", ""));
        Classify(actor, object, entry, item, stats);
        stats.rows[1].value = item.type;
        if (item.count > 1)
        {
            stats.rows.push_back(Row("Count", std::to_string(item.count)));
            stats.rows.push_back(Row("Weight", Fmt("%.1f", item.weight) + " each, " +
                                                   Fmt("%.1f", item.weight * static_cast<float>(item.count)) +
                                                   " in all"));
            stats.rows.push_back(Row("Value", std::to_string(item.value) + " each, " +
                                                  std::to_string(item.value * item.count) + " in all"));
        }
        else
        {
            stats.rows.push_back(Row("Weight", Fmt("%.1f", item.weight)));
            stats.rows.push_back(Row("Value", std::to_string(item.value)));
        }
        if (item.worn)
        {
            // The pin glyph beside the tick is added by MarkPins, which runs
            // after this scan and is the one that knows the pins.
            SheetRow equipped;
            equipped.label = "Equipped";
            equipped.icon = kGlyphTick;
            stats.rows.push_back(std::move(equipped));
        }
        item.detail.push_back(std::move(stats));

        // An enchantment, whether the record's or one put on at an arcane
        // enchanter: the entry answers for both.
        if (RE::EnchantmentItem *ench = entry ? entry->GetEnchantment() : nullptr)
        {
            item.enchanted = true;
            SheetSection section{"Enchantment", {}, {}};
            const std::string name = NameOf(ench);
            section.rows.push_back(Row("Name", name.empty() ? "(unnamed)" : name));
            if (const auto charge = entry->GetEnchantmentCharge())
                section.rows.push_back(Row("Charge", Fmt("%.0f%%", *charge)));
            item.detail.push_back(std::move(section));
            item.effects = EffectLines(ench);
        }

        out.push_back(std::move(item));
    }

    std::sort(out.begin(), out.end(), [](const InventoryItem &a, const InventoryItem &b) { return a.name < b.name; });
    return out;
}

} // namespace ft::game
