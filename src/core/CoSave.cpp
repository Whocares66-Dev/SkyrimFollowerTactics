#include "core/CoSave.h"

#include "core/Profile.h"

#include <cstring>
#include <format>

namespace ft
{

KeyedBy ChooseKeyRecord(bool baseInPlugin, bool referenceInPlugin) noexcept
{
    if (baseInPlugin)
        return KeyedBy::Base;
    return referenceInPlugin ? KeyedBy::Reference : KeyedBy::Dynamic;
}

std::string FollowerKey(std::string_view plugin, std::uint32_t localId)
{
    // No leading zeros, "A2C94", as a plugin's own id is written.
    return std::format("{}-{:X}", plugin, localId);
}

std::string DynamicKey(std::uint32_t referenceId)
{
    return std::format("dynamic-{:08X}", referenceId);
}

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
            contents.notes.emplace_back(
                log::Level::Warn,
                std::format("co-save record {:08X} is not one this build knows -- skipped", record.type));
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

void SavedProfiles::Load(std::vector<SavedFollower> followers)
{
    followers_ = std::move(followers);
}

void SavedProfiles::Forget() noexcept
{
    followers_.clear();
}

std::optional<std::string> SavedProfiles::Claim(std::string_view key)
{
    for (auto it = followers_.begin(); it != followers_.end(); ++it)
    {
        if (it->key != key)
            continue;
        std::string text = std::move(it->text);
        followers_.erase(it);
        return text;
    }
    return std::nullopt;
}

std::vector<SavedProfiles::Record> SavedProfiles::ToWrite(std::vector<Record> live) const
{
    std::vector<Record> out = std::move(live);
    out.reserve(out.size() + followers_.size());
    for (const SavedFollower &saved : followers_)
        out.push_back({saved.key, saved.text});
    return out;
}

const std::string *CoSaveContents::Follower(std::string_view key) const noexcept
{
    for (const SavedFollower &saved : followers)
        if (saved.key == key)
            return &saved.text;
    return nullptr;
}

} // namespace ft
