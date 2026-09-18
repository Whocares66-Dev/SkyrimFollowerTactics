#pragma once
// A session's files: the pair one launch of the game writes, and the archive
// the previous launch's pair moves into (dev/EVENTS.md "Sessions and
// files"). What can be decided without a disk is here, so it is tested: when
// a session began, read back from the first line it wrote; the name its pair
// is archived under; which archived pairs go; how much more a file may take.
// Moving, copying and reading file times are src/game/Log.cpp's.

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ft::log
{

// The archive keeps this many sessions. One session on 2026-09-14 wrote
// 151 KB of prose and 29.5 KB of events, so twenty is a few megabytes.
inline constexpr std::size_t kSessionsKept = 20;

// What one file may take in one session: a ceiling against a session left
// at debug for hours, far above anything a normal sitting writes.
inline constexpr std::uint64_t kCeilingBytes = 64ULL * 1024 * 1024;

// The words before the start on the prose log's first line, where the next
// launch looks for them. The line pattern carries only the time of day.
inline constexpr std::string_view kSessionBegan = "session began ";

// A moment to the second, in UTC.
struct UtcTime
{
    int year{0};
    int month{0};
    int day{0};
    int hour{0};
    int minute{0};
    int second{0};

    friend bool operator==(const UtcTime &, const UtcTime &) = default;
    friend auto operator<=>(const UtcTime &, const UtcTime &) = default;
};

// "2026-09-14T19:53:02", with or without milliseconds and a trailing Z, as
// the envelope's ts and the prose banner write it. Anything else is nothing.
[[nodiscard]] std::optional<UtcTime> ParseIsoTime(std::string_view text) noexcept;

// When a session began, from the first line of either of its files: the
// events file's session.started, or the prose log's banner. A file from
// before sessions were dated, or an empty one, gives nothing.
[[nodiscard]] std::optional<UtcTime> SessionStart(std::string_view firstLine);

// "2026-09-14-19-53-02": Crash Logger's format, so an archived session's
// name brackets any crash-<UTC>.log that fell inside it.
[[nodiscard]] std::string Stamp(const UtcTime &time);

// "FollowerTactics-<start>_<end>", or "FollowerTactics-_<end>" when the start
// could not be read. A session's .log and .events.jsonl share it.
[[nodiscard]] std::string ArchiveStem(const std::optional<UtcTime> &start, const UtcTime &end);

// The end an archive stem names; nothing for a name this did not make, which
// is what keeps the pruning away from anything else in the folder.
[[nodiscard]] std::optional<UtcTime> ArchiveEnd(std::string_view stem) noexcept;

// The stems to delete so that `keep` remain, the earliest ended first. A name
// this did not make is never among them and does not count.
[[nodiscard]] std::vector<std::string> StemsToDelete(std::vector<std::string> stems, std::size_t keep);

// Whether a line of `bytes` fits a file that had already taken `before`:
// Write; the line that crosses the ceiling, which says so instead of itself
// (Last); or None, for every line after it.
enum class Room : std::uint8_t
{
    Write,
    Last,
    None
};
[[nodiscard]] Room RoomFor(std::uint64_t before, std::uint64_t bytes, std::uint64_t ceiling) noexcept;

} // namespace ft::log
