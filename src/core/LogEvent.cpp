#include "LogEvent.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <nlohmann/json.hpp>

namespace ft::log
{
namespace
{

// ordered_json for the same reason Profile.cpp uses it: the envelope reads
// top to bottom in the order it was written, and two runs diff line by line.
using json = nlohmann::ordered_json;

constexpr std::string_view kPluginName = "FollowerTactics";

} // namespace

const char *ToString(Level level) noexcept
{
    switch (level)
    {
    case Level::Debug:
        return "debug";
    case Level::Info:
        return "info";
    case Level::Warn:
        return "warn";
    case Level::Error:
        return "error";
    }
    return "info";
}

bool ParseLevel(std::string_view text, Level &out) noexcept
{
    const auto same = [](std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != b[i])
                return false;
        return true;
    };

    static constexpr std::array<std::pair<std::string_view, Level>, 4> kNames{{
        {"debug", Level::Debug},
        {"info", Level::Info},
        {"warn", Level::Warn},
        {"error", Level::Error},
    }};

    for (const auto &[name, level] : kNames)
    {
        if (same(text, name))
        {
            out = level;
            return true;
        }
    }
    return false;
}

std::string Id(std::uint32_t formID)
{
    // Not fmt::format: core is built without fmt on the game-free presets.
    std::array<char, 16> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "0x%08X", formID);
    return written > 0 ? std::string(buffer.data(), static_cast<std::size_t>(written)) : std::string{};
}

Field::Field(std::string_view key, std::string value) : key_(key), value_(std::move(value))
{
}

Field::Field(std::string_view key, const char *value) : key_(key), value_(std::string(value ? value : ""))
{
}

Field::Field(std::string_view key, std::string_view value) : key_(key), value_(std::string(value))
{
}

Field::Field(std::string_view key, bool value) : key_(key), value_(value)
{
}

Field::Field(std::string_view key, const std::vector<std::uint32_t> &formIDs) : key_(key)
{
    std::vector<std::string> ids;
    ids.reserve(formIDs.size());
    for (const std::uint32_t id : formIDs)
        ids.push_back(Id(id));
    value_ = std::move(ids);
}

std::string FormatEvent(Level level, std::string_view event, std::string_view version, std::string_view timestamp,
                        std::span<const Field> fields)
{
    json line;
    line["ts"] = timestamp;
    line["level"] = ToString(level);
    line["plugin"] = kPluginName;
    line["version"] = version;
    line["event"] = event;

    for (const auto &field : fields)
    {
        const std::string key(field.key());
        std::visit([&](const auto &value) { line[key] = value; }, field.value());
    }

    // dump() with no indent is one line; nlohmann escapes what has to be
    // escaped, which is the whole reason a rule's label can be free text.
    return line.dump();
}

} // namespace ft::log
