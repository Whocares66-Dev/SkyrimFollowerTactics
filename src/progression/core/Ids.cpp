#include "progression/core/Ids.h"

#include <charconv>
#include <format>

namespace fp
{

std::string ToString(const FormKey &key)
{
    return std::format("{}|{:06X}", key.plugin, key.local);
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
