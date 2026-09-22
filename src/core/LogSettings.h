#pragma once
// What Data/SKSE/Plugins/FollowerTactics.ini says: the level the prose log
// is filtered by, and whether the events file is written. The game side
// finds and reads the file (game/Log.cpp); what its text means is decided
// here, where a stray comment, a misspelt value or a second assignment can
// be tested without a file. No Skyrim.
//
// Hand-parsed, and deliberately: the whole file is two keys, and a
// dependency (or a settings framework) to read them would cost more than
// it saves. If a third section ever appears this grows.

#include "LogEvent.h"

#include <string>
#include <string_view>
#include <vector>

namespace ft::log
{

struct IniSettings
{
    Level level{Level::Info};
    bool events{true};
    // `[Interface] language`: the panel's catalog, "zh-CN"; empty, or
    // "auto", follows the game's own language (core/I18n.h).
    std::string language;
};

// The file's text, as read. A `[Log]` section with `level` (error, warn,
// info or debug, any case) and `events` (true or false, in the usual
// spellings); `;` and `#` open a comment, blank lines and other sections
// are skipped, a later assignment replaces an earlier one. A value that
// does not read is said in `notes`, naming what stands instead -- the
// default, or the earlier valid assignment -- and is otherwise ignored:
// "events = yse" silently losing the sidecar would be found only by its
// absence.
[[nodiscard]] IniSettings ParseIniSettings(std::string_view text, std::vector<std::string> &notes);

} // namespace ft::log
