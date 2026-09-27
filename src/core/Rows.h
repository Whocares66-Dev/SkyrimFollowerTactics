#pragma once
// What the panel's lists decide about a row before anything is drawn:
// whether the filter and the category leave it on the list, which of its
// cells the filter searches, and what each equip cell of it says; and
// whether a rule's menu of them is divided under headings. The panel holds
// the filter text and the category and draws the answers (game/ui). No
// Skyrim, no ImGui.

#include "Marks.h"
#include "Views.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace ft
{

// Which columns an inventory list shows, by its category: the table lays
// them out by these, and the filter searches the cells they show.
struct ItemColumns
{
    bool weapons{false};     // a damage column
    bool armour{false};      // an armour column
    bool scrolls{false};     // cast and magnitude columns
    bool consumables{false}; // the second column says what the thing does, not its type
};
// `category` is the list's, or below zero for the All list.
[[nodiscard]] ItemColumns ColumnsOf(int category) noexcept;

// The cells of a row as its table shows them, which the filter searches:
// a filter on the name alone missed what the other columns are for.
[[nodiscard]] std::vector<std::string> ItemCells(const InventoryItem &item, int category);
// A voice entry's list has no school, level or cost column; the All list
// shows the school.
[[nodiscard]] std::vector<std::string> MagicCells(const MagicEntry &entry, int category, bool voice);
[[nodiscard]] std::vector<std::string> EffectCells(const EffectRow &row);

// Is the row on the list: in its category, with the filter's text in one
// of the cells the list shows for it.
[[nodiscard]] bool ItemShown(const InventoryItem &item, int category, std::string_view filter);
// A magic list is the voice's or the schools': an entry belongs to one of
// them.
[[nodiscard]] bool VoiceEntry(const MagicEntry &entry) noexcept;
[[nodiscard]] bool MagicShown(const MagicEntry &entry, int category, bool voice, std::string_view filter);
// An effect is listed when it is acting -- applied, and not inactive: the
// rows not greyed -- or when all effects are asked for; the filter's text is
// then looked for in its cells. Whether the game's own list hides it is
// not asked.
[[nodiscard]] bool EffectListed(const EffectRow &row, bool showAll) noexcept;
[[nodiscard]] bool EffectShown(const EffectRow &row, bool showAll, std::string_view filter);

// A column of one of the panel's tables. The number is the id the table
// gives its header, so a click says which column to order by.
enum class Column : unsigned
{
    Name = 1,
    Type,
    Damage,
    Armor,
    Weight,
    Value,
    Equipped,
    School,
    Level,
    Cast,
    Cost,
    Left,
    Right,
    Magnitude,
    Remaining,
    Source
};

// Which of two rows comes first in a column, ascending: -1, 0 or 1, a tie
// left to the name (SortRows). A column a list does not show answers as
// the name column does.
[[nodiscard]] int CompareItems(const InventoryItem &a, const InventoryItem &b, Column column);
[[nodiscard]] int CompareMagic(const MagicEntry &a, const MagicEntry &b, Column column);
[[nodiscard]] int CompareEffects(const EffectRow &a, const EffectRow &b, Column column);

// A row is dim where it is set aside by a pin, or -- for a spell -- above
// the follower's skill or a shout with no word unlocked: nothing the
// panel does to it can make them use it.
[[nodiscard]] bool Dimmed(const InventoryItem &item) noexcept;
[[nodiscard]] bool Dimmed(const MagicEntry &entry) noexcept;

// The equip cells of a row: the two hands and the worn column for an
// item; the two hands and the voice for a magic entry. A spell above
// their skill takes no hand at all, as the menus offer it for neither
// casting nor pinning; a voice entry takes the voice slot whatever the
// hands say.
[[nodiscard]] EquipCell LeftCell(const InventoryItem &item) noexcept;
[[nodiscard]] EquipCell RightCell(const InventoryItem &item) noexcept;
[[nodiscard]] EquipCell WornCell(const InventoryItem &item) noexcept;
[[nodiscard]] EquipCell LeftCell(const MagicEntry &entry) noexcept;
[[nodiscard]] EquipCell RightCell(const MagicEntry &entry) noexcept;
[[nodiscard]] EquipCell VoiceCell(const MagicEntry &entry) noexcept;

// A follower who has read every tome, or looted every barrow, has a menu
// nobody can find anything in, so a rule's long lists are divided under
// headings: a spell's school, a weapon's type, the part of the body armour
// is worn on. Under this many, the headings cost more than they save: a
// follower with four spells should not have to guess which heading one is
// under and open it to find out.
//
// Counted over what the menu is ABOUT to offer, not over everything the
// follower has: the lists are already cut to the target and the hand, so a
// follower with thirty spells of which four are Self gets a flat list under
// Self and the schools under an enemy.
inline constexpr std::size_t kGroupAtLeast = 10;

// The headings of `order` a menu of `items` is divided under, those with
// something under them, in that order; none for a list that stays flat.
// `headingOf` says which heading an item is under. Divided only where it
// pays: enough items that a flat list is hard to read, AND more than one
// heading to divide them into. Twelve Destruction spells under a lone
// Destruction heading is a layer that tells the player nothing they did not
// know before opening it.
template <typename Items, typename Heading, std::size_t N, typename HeadingOf>
[[nodiscard]] std::vector<Heading> MenuHeadings(const Items &items, const std::array<Heading, N> &order,
                                                HeadingOf headingOf)
{
    std::vector<Heading> used;
    for (const Heading &heading : order)
    {
        if (std::any_of(items.begin(), items.end(), [&](const auto &item) { return headingOf(item) == heading; }))
            used.push_back(heading);
    }
    if (std::size(items) < kGroupAtLeast || used.size() < 2)
        used.clear();
    return used;
}

} // namespace ft
