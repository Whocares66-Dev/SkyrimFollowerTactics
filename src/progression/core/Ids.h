#pragma once
// A form's identity across load orders: the plugin that defines it and its
// id within that plugin. A runtime FormID's top byte (or, for a light
// plugin, top twelve bits) is the plugin's place in the load order, which
// moves when the order does; the plugin's name and the local id do not.
// Everything the save keeps about a companion -- who they are, which perks
// were bought, which places were seen -- is filed under these. The game side
// turns them into forms and back (progression/game/Forms.cpp). No Skyrim.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace fp
{

struct FormKey
{
    std::string plugin;     // "Skyrim.esm"
    std::uint32_t local{0}; // 0x0A2C8E: the id within the plugin, no load-order index

    [[nodiscard]] bool Empty() const noexcept
    {
        return plugin.empty();
    }
    auto operator<=>(const FormKey &) const = default;
};

// "Skyrim.esm|0A2C8E": the plugin, a bar (a character no plugin name holds),
// six hex digits.
[[nodiscard]] std::string ToString(const FormKey &key);

// "0A2C8E": upper-case hex, zero-padded to `width` digits (more when the
// value needs them). Not std::format: clang-tidy's analyser falls over
// inside MSVC 14.42's <format> on calls from the core (Tactics'
// core/CoSave.cpp says the same), and the linter is one of the checks.
[[nodiscard]] std::string Hex(std::uint32_t value, int width);
// The reverse; none for anything that is not that shape.
[[nodiscard]] std::optional<FormKey> ParseFormKey(std::string_view text);

struct FormKeyHash
{
    [[nodiscard]] std::size_t operator()(const FormKey &key) const noexcept
    {
        return std::hash<std::string>{}(key.plugin) ^ (std::hash<std::uint32_t>{}(key.local) << 1);
    }
};

} // namespace fp
