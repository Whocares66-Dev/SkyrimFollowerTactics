#include "core/Table.h"

#include <cctype>

namespace ft
{

bool ContainsNoCase(std::string_view text, std::string_view needle) noexcept
{
    const auto same = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    return needle.empty() || std::search(text.begin(), text.end(), needle.begin(), needle.end(), same) != text.end();
}

bool AnyContains(const std::vector<std::string> &cells, std::string_view needle) noexcept
{
    return std::any_of(cells.begin(), cells.end(),
                       [needle](const std::string &cell) { return ContainsNoCase(cell, needle); });
}

} // namespace ft
