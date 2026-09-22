#include "progression/core/Ids.h"

#include <charconv>

namespace fp
{

std::string Hex(std::uint32_t value, int width)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    for (; value != 0; value >>= 4)
        out.insert(out.begin(), kDigits[value & 0xF]);
    if (static_cast<int>(out.size()) < width)
        out.insert(0, static_cast<std::size_t>(width) - out.size(), '0');
    return out;
}

std::string ToString(const FormKey &key)
{
    return key.plugin + "|" + Hex(key.local, 6);
}

std::optional<FormKey> ParseFormKey(std::string_view text)
{
    const auto bar = text.rfind('|');
    if (bar == std::string_view::npos || bar == 0 || bar + 1 >= text.size())
        return std::nullopt;
    const std::string_view hex = text.substr(bar + 1);
    std::uint32_t local = 0;
    const auto [end, error] = std::from_chars(hex.data(), hex.data() + hex.size(), local, 16);
    if (error != std::errc{} || end != hex.data() + hex.size() || local > 0xFFFFFF)
        return std::nullopt;
    return FormKey{std::string(text.substr(0, bar)), local};
}

} // namespace fp
