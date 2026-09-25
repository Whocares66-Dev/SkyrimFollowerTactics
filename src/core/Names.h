#pragma once
// The order a person looks for things in: by the name shown. One rule for
// every list and menu sorted by name -- the spells, the potions, the
// statuses, the perks, the Weapon heading's entries -- so none orders
// differently from the next. No Skyrim.

#include <algorithm>
#include <string_view>

namespace ft
{

// Is `a` before `b` by name? Letters compared without their case, as a
// reader looks them up: "iron" beside "Iron", not after "Z". Names equal
// but for case then by their bytes, so each has one place. Only ASCII
// letters fold; a name in another script sorts by its UTF-8 bytes, which
// keeps each language's names together and in one fixed order.
[[nodiscard]] bool NameBefore(std::string_view a, std::string_view b) noexcept;

// Sort by the name `name` gives each item, items of one name kept in the
// order they came.
template <class Range, class Name> void SortByName(Range &items, Name &&name)
{
    std::ranges::stable_sort(items, [&](const auto &a, const auto &b) { return NameBefore(name(a), name(b)); });
}

} // namespace ft
