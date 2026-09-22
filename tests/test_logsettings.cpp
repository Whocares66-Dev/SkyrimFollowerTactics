// The ini's two keys, as text. No Skyrim; the game side reads the file.

#include <catch2/catch_test_macros.hpp>

#include "core/LogSettings.h"

#include <string>
#include <vector>

using ft::log::IniSettings;
using ft::log::Level;
using ft::log::ParseIniSettings;

namespace
{

IniSettings Parse(std::string_view text, std::vector<std::string> &notes)
{
    notes.clear();
    return ParseIniSettings(text, notes);
}

} // namespace

TEST_CASE("no text, or nothing in it, is the defaults", "[ini]")
{
    std::vector<std::string> notes;
    IniSettings s = Parse("", notes);
    REQUIRE(s.level == Level::Info);
    REQUIRE(s.events);
    REQUIRE(notes.empty());

    s = Parse("; a comment\n# another\n\n   \n[Log]\n", notes);
    REQUIRE(s.level == Level::Info);
    REQUIRE(s.events);
    REQUIRE(notes.empty());
}

TEST_CASE("the two keys, whatever the whitespace, line ending or case", "[ini]")
{
    std::vector<std::string> notes;
    const IniSettings s = Parse("[Log]\r\n  level =  DEBUG  \r\nevents=off\r\n", notes);
    REQUIRE(s.level == Level::Debug);
    REQUIRE_FALSE(s.events);
    REQUIRE(notes.empty());
    // A last line with no newline is still a line.
    REQUIRE(Parse("[Log]\nlevel = warn", notes).level == Level::Warn);
}

TEST_CASE("only the Log section counts", "[ini]")
{
    std::vector<std::string> notes;
    IniSettings s = Parse("level = debug\n[Other]\nlevel = error\nevents = false\n[Log]\nlevel = warn\n", notes);
    REQUIRE(s.level == Level::Warn);
    REQUIRE(s.events);
    // A section header with no closing bracket names no section.
    s = Parse("[Log\nlevel = debug\n", notes);
    REQUIRE(s.level == Level::Info);
    // A line with no '=' is skipped.
    s = Parse("[Log]\nlevel debug\nevents\n", notes);
    REQUIRE(s.level == Level::Info);
    REQUIRE(notes.empty());
}

TEST_CASE("the spellings of true and false", "[ini]")
{
    std::vector<std::string> notes;
    for (const char *yes : {"1", "true", "TRUE", "True", "yes", "on"})
        REQUIRE(Parse(std::string("[Log]\nevents = false\nevents = ") + yes, notes).events);
    for (const char *no : {"0", "false", "FALSE", "False", "no", "off"})
        REQUIRE_FALSE(Parse(std::string("[Log]\nevents = ") + no, notes).events);
    REQUIRE(notes.empty());
}

TEST_CASE("a value that does not read is said, naming what stands", "[ini]")
{
    std::vector<std::string> notes;
    IniSettings s = Parse("[Log]\nlevel = loud\nevents = yse\n", notes);
    REQUIRE(s.level == Level::Info);
    REQUIRE(s.events);
    REQUIRE(notes.size() == 2);
    REQUIRE(notes[0] == "level \"loud\" is not one of error/warn/info/debug -- using info");
    REQUIRE(notes[1] == "events \"yse\" is not true or false -- using true");

    // An earlier valid assignment stands, and the note says so rather than
    // claiming the default.
    s = Parse("[Log]\nlevel = debug\nlevel = loud\nevents = off\nevents = maybe\n", notes);
    REQUIRE(s.level == Level::Debug);
    REQUIRE_FALSE(s.events);
    REQUIRE(notes.size() == 2);
    REQUIRE(notes[0] == "level \"loud\" is not one of error/warn/info/debug -- using debug");
    REQUIRE(notes[1] == "events \"maybe\" is not true or false -- using false");
}

TEST_CASE("a later assignment replaces an earlier one", "[ini]")
{
    std::vector<std::string> notes;
    const IniSettings s = Parse("[Log]\nlevel = error\nlevel = warn\nevents = false\nevents = true\n", notes);
    REQUIRE(s.level == Level::Warn);
    REQUIRE(s.events);
}

TEST_CASE("the panel's language, or auto for the game's", "[ini]")
{
    std::vector<std::string> notes;
    REQUIRE(Parse("", notes).language.empty());
    REQUIRE(Parse("[Interface]\nlanguage = auto\n", notes).language.empty());
    REQUIRE(Parse("[Interface]\nlanguage = zh-CN\n", notes).language == "zh-CN");
    // Only under its own section.
    REQUIRE(Parse("[Log]\nlanguage = zh-CN\n", notes).language.empty());
}
