#pragma once
// Moving the previous session's pair of files into the archive, and
// pruning it (dev/EVENTS.md "Sessions and files"). Which name a pair takes
// and which old ones go are Sessions.h's; this is the disk work around
// them -- reading the first line, reading when a file was last written,
// the move, the copy a move refused falls back to, and the deletes. No
// Skyrim: the game passes the folder it logs into (game/Log.cpp).

#include "Sessions.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ft::log
{

// A session's two files, and the ending each keeps in the archive. The
// events file first: its first line dates the session most exactly.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 2> kSessionFiles{{
    {"FollowerTactics.events.jsonl", ".events.jsonl"},
    {"FollowerTactics.log", ".log"},
}};

// The folder the archive lives in, under the log folder.
inline constexpr std::string_view kArchiveFolder = "FollowerTactics";

// When a file was last written, to the second; nothing when it cannot be
// read. Not when it was created: a truncated file keeps its creation
// time, and NTFS gives a file made under a name just renamed away the old
// file's.
[[nodiscard]] std::optional<UtcTime> WrittenAt(const std::filesystem::path &file);

// The previous session's pair moved into the archive under its start and
// end, and the archive pruned to `keep` sessions. Nothing at all when
// neither file is there -- a first run. What could not be moved, and what
// was copied instead, is said in `notes`, for the log that is not open
// yet.
void ArchiveSession(const std::filesystem::path &directory, std::vector<std::string> &notes,
                    std::size_t keep = kSessionsKept);

} // namespace ft::log
