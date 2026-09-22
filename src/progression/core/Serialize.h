#pragma once
// What goes into the SKSE co-save, and how it is read back. One record per
// companion, its payload the companion's JSON, so a record that fails to
// read costs that companion and nobody else; one record for the settings.
// Every length is checked before it is trusted, and a record is read whole
// or not at all -- the two native prior-art mods both misread every record
// after an actor that failed to resolve (dev/PRIOR_ART.md). The game side
// owns SKSE's interface and hands whole records in and out
// (progression/game/Persistence.cpp). No Skyrim.

#include "progression/core/Companion.h"
#include "progression/core/Settings.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fp
{

[[nodiscard]] constexpr std::uint32_t RecordTag(char a, char b, char c, char d) noexcept
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

// In Tactics' co-save block, beside its own records (game/Profiles.cpp):
// none of these types is one of Tactics'.
inline constexpr std::uint32_t kCompanionRecord = RecordTag('C', 'O', 'M', 'P');
inline constexpr std::uint32_t kSettingsRecord = RecordTag('P', 'S', 'E', 'T');

[[nodiscard]] constexpr bool IsProgressionRecord(std::uint32_t type) noexcept
{
    return type == kCompanionRecord || type == kSettingsRecord;
}
// The schema of both. A record from a newer build says so before it is read.
inline constexpr std::uint32_t kSchema = 1;
// A companion's record is a few kilobytes; one claiming megabytes is not ours.
inline constexpr std::uint32_t kMaxRecordBytes = 4u << 20;

[[nodiscard]] std::string WriteCompanion(const Companion &c);
// None when the text is not a companion this build can read; `why` says
// what was wrong. Fields a newer build added are ignored; fields missing
// take their defaults.
[[nodiscard]] std::optional<Companion> ReadCompanion(std::string_view text, std::string *why = nullptr);

[[nodiscard]] std::string WriteSettings(const Settings &s);
[[nodiscard]] std::optional<Settings> ReadSettings(std::string_view text);

struct CoSaveRecord
{
    std::uint32_t type{0};
    std::uint32_t version{0};
    std::string payload;
};

struct CoSaveContents
{
    std::vector<Companion> companions;
    std::optional<Settings> settings;
    std::vector<std::string> notes; // every record skipped, and why
};

[[nodiscard]] std::vector<CoSaveRecord> PackCoSave(std::span<const Companion> companions, const Settings &settings);
[[nodiscard]] CoSaveContents UnpackCoSave(std::span<const CoSaveRecord> records);

} // namespace fp
