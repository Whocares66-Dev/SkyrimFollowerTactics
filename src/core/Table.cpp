#include "core/Table.h"

#include <cctype>
#include <cstdio>

namespace ft
{

std::string Fmt(const char *format, double value)
{
    char buffer[64];
    const int written = std::snprintf(buffer, sizeof buffer, format, value);
    return written > 0 ? std::string(buffer, static_cast<std::size_t>(written)) : std::string{};
}

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
