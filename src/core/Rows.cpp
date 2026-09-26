#include "core/Rows.h"

#include "core/Table.h"

namespace ft
{

ItemColumns ColumnsOf(int category) noexcept
{
    const auto is = [category](ItemCategory kind) { return category == static_cast<int>(kind); };
    ItemColumns columns;
    columns.weapons = is(ItemCategory::Weapons) || is(ItemCategory::Arrows);
    columns.armour = is(ItemCategory::Armor);
    columns.scrolls = is(ItemCategory::Scrolls);
    columns.consumables = columns.scrolls || is(ItemCategory::Potions) || is(ItemCategory::Poisons) ||
                          is(ItemCategory::Food) || is(ItemCategory::Ingredients);
    return columns;
}

std::vector<std::string> ItemCells(const InventoryItem &item, int category)
{
    const ItemColumns columns = ColumnsOf(category);
    std::vector<std::string> cells{item.name, columns.consumables ? item.effect : item.type, Fmt("%.1f", item.weight),
                                   std::to_string(item.value)};
    if (const float stat = columns.weapons ? item.damage : columns.armour ? item.armor : 0.0f; stat > 0.0f)
        cells.push_back(Fmt("%.0f", stat));
    if (columns.scrolls)
    {
        cells.push_back(item.cast);
        if (item.magnitude > 0.0f)
            cells.push_back(Fmt("%.0f", item.magnitude));
    }
    return cells;
}

std::vector<std::string> MagicCells(const MagicEntry &entry, int category, bool voice)
{
    std::vector<std::string> cells{entry.name, entry.type, entry.cast};
    // The three a voice list has no room for; School only where the All
    // list shows it.
    if (!voice)
    {
        if (category < 0)
            cells.push_back(entry.school);
        cells.push_back(entry.level);
        cells.push_back(entry.cost);
    }
    if (entry.magnitude > 0.0f)
        cells.push_back(Fmt("%.0f", entry.magnitude));
    return cells;
}

std::vector<std::string> EffectCells(const EffectRow &row)
{
    std::vector<std::string> cells{row.name, row.remainingText, row.source};
    if (row.magnitude != 0.0f)
        cells.push_back(Fmt("%.0f", row.magnitude));
    return cells;
}

bool ItemShown(const InventoryItem &item, int category, std::string_view filter)
{
    if (category >= 0 && static_cast<int>(item.category) != category)
        return false;
    return AnyContains(ItemCells(item, category), filter);
}

bool VoiceEntry(const MagicEntry &entry) noexcept
{
    return entry.category == MagicCategory::Shouts || entry.category == MagicCategory::Powers;
}

bool MagicShown(const MagicEntry &entry, int category, bool voice, std::string_view filter)
{
    if (VoiceEntry(entry) != voice)
        return false;
    if (category >= 0 && static_cast<int>(entry.category) != category)
        return false;
    return AnyContains(MagicCells(entry, category, voice), filter);
}

bool EffectListed(const EffectRow &row, bool showAll) noexcept
{
    return showAll || (row.applied && row.active);
}

bool EffectShown(const EffectRow &row, bool showAll, std::string_view filter)
{
    return EffectListed(row, showAll) && AnyContains(EffectCells(row), filter);
}

bool Dimmed(const InventoryItem &item) noexcept
{
    return item.setAside;
}

bool Dimmed(const MagicEntry &entry) noexcept
{
    return entry.setAside || entry.aboveSkill || entry.locked;
}

EquipCell LeftCell(const InventoryItem &item) noexcept
{
    return {item.handItem && !item.rightOnly, Dimmed(item), item.equippedLeft, item.pinnedLeft, item.banned};
}

EquipCell RightCell(const InventoryItem &item) noexcept
{
    return {item.handItem && !item.leftOnly, Dimmed(item), item.equippedRight, item.pinnedRight, item.banned};
}

EquipCell WornCell(const InventoryItem &item) noexcept
{
    return {!item.handItem, Dimmed(item), item.worn, item.pinned, item.banned};
}

EquipCell LeftCell(const MagicEntry &entry) noexcept
{
    return {VoiceEntry(entry) || (entry.leftAllowed && !entry.aboveSkill), Dimmed(entry), entry.equippedLeft,
            entry.pinnedLeft, entry.banned};
}

EquipCell RightCell(const MagicEntry &entry) noexcept
{
    return {VoiceEntry(entry) || (entry.rightAllowed && !entry.aboveSkill), Dimmed(entry), entry.equippedRight,
            entry.pinnedRight, entry.banned};
}

EquipCell VoiceCell(const MagicEntry &entry) noexcept
{
    return {!entry.locked, Dimmed(entry), entry.equipped, entry.pinned, entry.banned};
}

} // namespace ft

namespace ft
{

int CompareItems(const InventoryItem &a, const InventoryItem &b, Column column)
{
    switch (column)
    {
    case Column::Type:
        // The consumables' lists show the effect in this column.
        return a.effect.empty() && b.effect.empty() ? a.type.compare(b.type) : a.effect.compare(b.effect);
    case Column::Damage:
        return Compare(a.damage, b.damage);
    case Column::Armor:
        return Compare(a.armor, b.armor);
    case Column::Cast:
        return a.cast.compare(b.cast);
    case Column::Magnitude:
        return Compare(a.magnitude, b.magnitude);
    case Column::Weight:
        return Compare(a.weight, b.weight);
    case Column::Value:
        return Compare(a.value, b.value);
    case Column::Equipped:
        return Compare(CellRank(WornCell(a)), CellRank(WornCell(b)));
    case Column::Left:
        return Compare(CellRank(LeftCell(a)), CellRank(LeftCell(b)));
    case Column::Right:
        return Compare(CellRank(RightCell(a)), CellRank(RightCell(b)));
    case Column::Name:
    default:
        return a.name.compare(b.name);
    }
}

int CompareMagic(const MagicEntry &a, const MagicEntry &b, Column column)
{
    switch (column)
    {
    case Column::School:
        return a.school.compare(b.school);
    case Column::Type:
        return a.type.compare(b.type);
    case Column::Level:
        return Compare(a.levelValue, b.levelValue);
    case Column::Cast:
        // The delivery behind the word first, so Self, Touch and Target
        // group; the word itself only tells two of one delivery apart.
        return a.castValue != b.castValue ? Compare(a.castValue, b.castValue) : a.cast.compare(b.cast);
    case Column::Cost:
        return Compare(a.costValue, b.costValue);
    case Column::Magnitude:
        return Compare(a.magnitude, b.magnitude);
    case Column::Equipped:
        return Compare(CellRank(VoiceCell(a)), CellRank(VoiceCell(b)));
    case Column::Left:
        return Compare(CellRank(LeftCell(a)), CellRank(LeftCell(b)));
    case Column::Right:
        return Compare(CellRank(RightCell(a)), CellRank(RightCell(b)));
    case Column::Name:
    default:
        return a.name.compare(b.name);
    }
}

int CompareEffects(const EffectRow &a, const EffectRow &b, Column column)
{
    // No duration sorts after every duration: it is the one that never
    // runs out.
    const auto left = [](const EffectRow &e) { return e.remaining < 0.0f ? 1.0e9f : e.remaining; };
    switch (column)
    {
    case Column::Magnitude:
        return Compare(a.magnitude, b.magnitude);
    case Column::Remaining:
        return Compare(left(a), left(b));
    case Column::Source:
        return a.source.compare(b.source);
    case Column::Name:
    default:
        return a.name.compare(b.name);
    }
}

} // namespace ft
