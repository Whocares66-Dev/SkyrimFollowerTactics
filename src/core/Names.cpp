#include "core/Names.h"

#include <cstddef>

namespace ft
{
namespace
{

// ASCII only, and not the C library's tolower: that one asks the locale,
// and a name's place must not depend on the machine it is sorted on.
constexpr unsigned char Fold(char c) noexcept
{
    const auto u = static_cast<unsigned char>(c);
    return u >= 'A' && u <= 'Z' ? static_cast<unsigned char>(u - 'A' + 'a') : u;
}

} // namespace

bool NameBefore(std::string_view a, std::string_view b) noexcept
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        const unsigned char x = Fold(a[i]);
        const unsigned char y = Fold(b[i]);
        if (x != y)
            return x < y;
    }
    if (a.size() != b.size())
        return a.size() < b.size();
    return a < b;
}

} // namespace ft
