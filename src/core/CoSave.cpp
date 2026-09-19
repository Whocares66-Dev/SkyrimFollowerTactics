#include "core/CoSave.h"

#include "core/Profile.h"

#include <cstring>

namespace ft
{
namespace
{

// "50524F46": eight hex digits, for a record type in a note. Not
// std::format: clang-tidy's analyser falls over inside the MSVC 14.42
// <format> internals on any call from here, and the linter is one of the
// checks (CLAUDE.md).
std::string Hex8(std::uint32_t value)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i, value >>= 4)
        out[static_cast<std::size_t>(i)] = kDigits[value & 0xF];
    return out;
}

} // namespace

void PutString(std::string &out, std::string_view s)
{
    const auto length = static_cast<std::uint32_t>(s.size());
    char bytes[sizeof length];
    std::memcpy(bytes, &length, sizeof length);
    out.append(bytes, sizeof length);
    out.append(s);
}

std::optional<std::string_view> TakeString(std::string_view &in) noexcept
{
    std::uint32_t length = 0;
    if (in.size() < sizeof length)
        return std::nullopt;
    std::memcpy(&length, in.data(), sizeof length);
    if (in.size() - sizeof length < length)
        return std::nullopt;
    const std::string_view s = in.substr(sizeof length, length);
    in.remove_prefix(sizeof length + length);
    return s;
}

std::string PackFollower(std::string_view key, std::string_view text)
{
    std::string out;
    out.reserve(2 * sizeof(std::uint32_t) + key.size() + text.size());
    PutString(out, key);
    PutString(out, text);
    return out;
}

std::string PackSettings(std::string_view text)
{
    std::string out;
    PutString(out, text);
    return out;
}

CoSaveContents UnpackCoSave(std::span<const CoSaveRecord> records)
{
    CoSaveContents contents;
    for (const CoSaveRecord &record : records)
    {
        std::string_view rest = record.payload;
        if (record.type == kSettingsRecord)
        {
            const auto text = TakeString(rest);
            if (!text)
            {
                contents.notes.emplace_back(log::Level::Error,
                                            "the settings record is cut short -- the defaults stand");
                continue;
            }
            contents.settings = std::string(*text);
            continue;
        }
        if (record.type != kFollowerRecord)
        {
            contents.notes.emplace_back(log::Level::Warn, "co-save record " + Hex8(record.type) +
                                                              " is not one this build knows -- skipped");
            continue;
        }
        const auto key = TakeString(rest);
        const auto text = key ? TakeString(rest) : std::nullopt;
        if (!text)
        {
            contents.notes.emplace_back(log::Level::Error, "a co-save record is cut short -- skipped");
            continue;
        }
        if (record.version > static_cast<std::uint32_t>(kProfileSchema))
            contents.notes.emplace_back(log::Level::Warn, std::string(*key) + ": saved by a newer build (schema " +
                                                              std::to_string(record.version) +
                                                              ") -- reading what this one understands");
        bool replaced = false;
        for (SavedFollower &saved : contents.followers)
        {
            if (saved.key == *key)
            {
                saved.text = std::string(*text);
                replaced = true;
                break;
            }
        }
        if (!replaced)
            contents.followers.push_back({std::string(*key), std::string(*text)});
    }
    return contents;
}

const std::string *CoSaveContents::Follower(std::string_view key) const noexcept
{
    for (const SavedFollower &saved : followers)
        if (saved.key == key)
            return &saved.text;
    return nullptr;
}

} // namespace ft
