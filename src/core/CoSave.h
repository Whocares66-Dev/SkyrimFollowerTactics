#pragma once
// The co-save's records: how a follower's tactics and the player's
// settings are framed as bytes, and what a loaded set of records means.
// The game side owns SKSE's serialization interface -- it opens records,
// writes and reads the bytes, and logs (game/Profiles.cpp); the framing
// and the reading policy are here, where a cut-short record or a length
// that runs past its record can be tested without a save. No Skyrim.
//
// A record is a type, a version and a payload. The follower record's
// payload is two length-prefixed strings, the key and the JSON text; the
// settings record's is one, the JSON text. A length is four bytes,
// little-endian as the machine is, then the bytes.

#include "LogEvent.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ft
{

// A record type as SKSE names them: four characters, the first the most
// significant, as a multi-character constant reads on every compiler the
// plugin builds with.
[[nodiscard]] constexpr std::uint32_t RecordTag(char a, char b, char c, char d) noexcept
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

// One record per follower: the key, then the JSON text. The record's
// version is the format's schema number (kProfileSchema), so a record
// from a newer build says so before it is parsed.
inline constexpr std::uint32_t kFollowerRecord = RecordTag('P', 'R', 'O', 'F');
// One record for the player's own choices, which belong to no follower.
inline constexpr std::uint32_t kSettingsRecord = RecordTag('S', 'E', 'T', 'T');

// The most a record may claim to be. A record is a few kilobytes of JSON;
// a length in the megabytes is a save that is not ours to read, and taken
// at its word it would be a gigabyte allocated on a load screen.
inline constexpr std::uint32_t kMaxRecordBytes = 16u << 20;

// A length-prefixed string onto the end of `out`.
void PutString(std::string &out, std::string_view s);
// The string at the front of `in`, which moves past it; none when the
// length is cut short or runs past the end, and `in` is then left as it
// was.
[[nodiscard]] std::optional<std::string_view> TakeString(std::string_view &in) noexcept;

// The key a follower's record is filed under: the plugin that defines
// the record and its id within it, "Skyrim.esm-A2C94", which survives a
// load order change where a runtime id does not. The base record when it
// is a plugin's, else the placed reference when that is, else the
// reference's runtime id, "dynamic-FF000DE0", as good as it gets.
enum class KeyedBy : std::uint8_t
{
    Base,
    Reference,
    Dynamic
};
[[nodiscard]] KeyedBy ChooseKeyRecord(bool baseInPlugin, bool referenceInPlugin) noexcept;
[[nodiscard]] std::string FollowerKey(std::string_view plugin, std::uint32_t localId);
[[nodiscard]] std::string DynamicKey(std::uint32_t referenceId);

// The payloads.
[[nodiscard]] std::string PackFollower(std::string_view key, std::string_view text);
[[nodiscard]] std::string PackSettings(std::string_view text);

// A record as read: what SKSE says of it, and its bytes in full.
struct CoSaveRecord
{
    std::uint32_t type{0};
    std::uint32_t version{0};
    std::string payload;
};

// What the loaded records hold. A follower's text by key, each key once,
// the later of two records under one key winning; the settings text, the
// last one found; and what was skipped or doubted, at the level the log
// should say it.
struct SavedFollower
{
    std::string key;
    std::string text;
};
struct CoSaveContents
{
    std::optional<std::string> settings;
    std::vector<SavedFollower> followers;
    std::vector<std::pair<log::Level, std::string>> notes;

    // The text under this key, or null.
    [[nodiscard]] const std::string *Follower(std::string_view key) const noexcept;
};
[[nodiscard]] CoSaveContents UnpackCoSave(std::span<const CoSaveRecord> records);

// The follower records a loaded save holds, until a follower claims
// theirs. Whatever is still here when the game saves is written back as
// it came, so a dismissed follower's tactics survive any number of saves
// made while they are away; and a load, or a new game, forgets the lot
// before the next save's records arrive.
class SavedProfiles
{
  public:
    void Load(std::vector<SavedFollower> followers);
    void Forget() noexcept;

    // The text filed under this key, taken out: from here on the follower
    // is live and is written from the session's state, so a second claim
    // finds nothing. Two references of one base share a key, and the
    // first the tick sees claims it.
    [[nodiscard]] std::optional<std::string> Claim(std::string_view key);
    [[nodiscard]] std::size_t Carried() const noexcept
    {
        return followers_.size();
    }

    // What the save is written from: what the live followers have now,
    // then what is still carried, each once and in that order.
    struct Record
    {
        std::string key;
        std::string text;
    };
    [[nodiscard]] std::vector<Record> ToWrite(std::vector<Record> live) const;

  private:
    std::vector<SavedFollower> followers_;
};

} // namespace ft
