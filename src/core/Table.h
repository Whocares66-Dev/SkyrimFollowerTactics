#pragma once
// What a list in the panel decides without drawing: whether a row's cells
// hold the filter's text, and the order the rows go in for the column the
// header asks for. The panel reads the filter box and the table's sort
// spec (game/UI.cpp) and asks here; the matching and the ordering are
// tested against rows built by hand. No Skyrim, no ImGui.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace ft
{

// Does the text hold the needle, whatever the case? An empty needle is in
// everything.
[[nodiscard]] bool ContainsNoCase(std::string_view text, std::string_view needle) noexcept;

// Does any of a row's cells, as its table shows them, hold the filter's
// text? A filter on the name alone missed what the other columns are for:
// "Fire" among the spells, "Heavy" among the armour.
[[nodiscard]] bool AnyContains(const std::vector<std::string> &cells, std::string_view needle) noexcept;

// Which of two numbers comes first, as a column's compare answers it: -1,
// 0 or 1.
[[nodiscard]] constexpr int Compare(double a, double b) noexcept
{
    return a < b ? -1 : (a > b ? 1 : 0);
}

// Put the rows in the order the header asks for. Every list sorts the
// same way and only the column differs, so the column is all a list says:
// `compare` answers -1, 0 or 1 for two rows in the column named. Ties go
// by name ascending whichever way the column points, so rows that are
// equal under it keep one order rather than shuffling as the sort runs
// again; rows equal by name too keep the order they came in.
template <typename Row, typename Column, typename Compared>
void SortRows(std::vector<const Row *> &rows, const Compared &compare, Column column, bool ascending)
{
    std::stable_sort(rows.begin(), rows.end(), [&](const Row *a, const Row *b) {
        const int c = compare(*a, *b, column);
        if (c == 0)
            return a->name < b->name;
        return ascending ? c < 0 : c > 0;
    });
}

} // namespace ft
