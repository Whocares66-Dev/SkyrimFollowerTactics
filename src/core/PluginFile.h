#pragma once
// A plugin file's bytes, read as the game reads them: record and group
// headers, the file's masters, a record's subrecords, and the forms an
// attached script's properties name (VMAD). What the engine does not keep
// after it loads -- a script property is handed to the script engine and
// kept nowhere readable -- is read back from the file (game/Toggles.cpp).
//
// Form IDs here are the file's own numbering: the high byte is an index
// into the file's masters, and one past the last is the file itself.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ft
{

// The first 24 bytes of a record or a group.
struct PluginHeader
{
    std::array<char, 4> type{};
    // A record's: its data, after the header. A group's: the whole group,
    // header included.
    std::uint32_t size{0};
    // Bytes 8 to 11: a record's flags, or a group's label -- the record
    // type, for a top group.
    std::uint32_t flags{0};
    std::array<char, 4> label{};
    // A record's form ID; a group's type.
    std::uint32_t formID{0};

    [[nodiscard]] bool IsGroup() const noexcept;
    [[nodiscard]] bool Is(std::string_view t) const noexcept;
    [[nodiscard]] bool Labelled(std::string_view t) const noexcept;
};

inline constexpr std::size_t kPluginHeaderSize = 24;
// A record whose data is zlib-compressed.
inline constexpr std::uint32_t kRecordCompressed = 0x00040000;

[[nodiscard]] std::optional<PluginHeader> ReadPluginHeader(std::span<const std::uint8_t> bytes) noexcept;

// The masters a file's header record (TES4) names, in order: master i is
// the high byte i of the file's own form IDs.
[[nodiscard]] std::vector<std::string> PluginMasters(std::span<const std::uint8_t> headerData);

struct PluginRecord
{
    std::uint32_t formID{0};
    std::uint32_t flags{0};
    std::span<const std::uint8_t> data;

    [[nodiscard]] bool Compressed() const noexcept
    {
        return (flags & kRecordCompressed) != 0;
    }
};

// The records a group holds, header included, in order. Groups within it
// are skipped; a group cut short ends the list where it stops.
[[nodiscard]] std::vector<PluginRecord> RecordsIn(std::span<const std::uint8_t> group);

// The first subrecord of that type in a record's data, with an XXXX before
// it giving a size too large for the usual field. Empty for none.
[[nodiscard]] std::span<const std::uint8_t> Subrecord(std::span<const std::uint8_t> data, std::string_view type);

// One form an attached script's property names: an Object property, or an
// element of an Object array.
struct ScriptObjectProperty
{
    std::string script;
    std::string property;
    std::uint32_t formID{0};
};

// Every form the scripts in a VMAD subrecord name by their properties, in
// order. Stops at anything it cannot read -- a property type Skyrim's
// scripts do not have -- with what it had read by then.
[[nodiscard]] std::vector<ScriptObjectProperty> ScriptObjectProperties(std::span<const std::uint8_t> vmad);

} // namespace ft
