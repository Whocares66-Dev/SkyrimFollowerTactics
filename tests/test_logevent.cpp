// The structured log line: the envelope's shape, how each kind of value is
// spelled, and that free text cannot break the line. No Skyrim here -- the
// clock and the actor's name are the game layer's, and are passed in.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "core/LogEvent.h"

#include <string>

using namespace ft;

namespace
{

constexpr std::string_view kVersion = "0.1.0";
constexpr std::string_view kStamp = "2026-09-09T14:02:11.400Z";

nlohmann::json Parse(const std::string &line)
{
    return nlohmann::json::parse(line);
}

} // namespace

TEST_CASE("the envelope carries the same five fields every time", "[logevent]")
{
    const auto line = log::FormatEvent(log::Level::Info, "rule.fired", kVersion, kStamp, {});
    const auto j = Parse(line);

    CHECK(j["ts"] == "2026-09-09T14:02:11.400Z");
    CHECK(j["level"] == "info");
    CHECK(j["plugin"] == "FollowerTactics");
    CHECK(j["version"] == "0.1.0");
    CHECK(j["event"] == "rule.fired");
}

TEST_CASE("one line, whatever it carries", "[logevent]")
{
    const auto line = log::FormatEvent(log::Level::Warn, "rule.actionFailed", kVersion, kStamp,
                                       {{"ruleName", "heal\nwith\ta break"}, {"reason", "no potion"}});
    CHECK(line.find('\n') == std::string::npos);
    CHECK(Parse(line)["ruleName"] == "heal\nwith\ta break");
}

TEST_CASE("a label the player typed cannot break the line", "[logevent]")
{
    // Rule labels are free text: the panel lets the player write anything,
    // and a quote or a backslash in one must not produce an unparseable line.
    const auto line =
        log::FormatEvent(log::Level::Info, "rule.fired", kVersion, kStamp, {{"ruleName", R"(say "hi" \ then heal)"}});

    REQUIRE_NOTHROW(Parse(line));
    CHECK(Parse(line)["ruleName"] == R"(say "hi" \ then heal)");
}

TEST_CASE("each kind of value is spelled the way a query expects", "[logevent]")
{
    const auto j = Parse(log::FormatEvent(log::Level::Info, "package.armed", kVersion, kStamp,
                                          {{"slot", 3},
                                           {"count", std::size_t{2}},
                                           {"durationS", 1.5},
                                           {"held", true},
                                           {"reason", "cast"},
                                           {"spellFormId", log::Id(0x0007E8C1)}}));

    CHECK(j["slot"] == 3);
    CHECK(j["slot"].is_number_integer());
    CHECK(j["count"] == 2);
    CHECK(j["durationS"] == 1.5);
    CHECK(j["held"] == true);
    CHECK(j["held"].is_boolean());
    CHECK(j["reason"] == "cast");
    CHECK(j["spellFormId"] == "0x0007E8C1");
    CHECK(j["spellFormId"].is_string());
}

TEST_CASE("a number is written to milliseconds, not to a float's noise", "[logevent]")
{
    // What a float brings with it when it widens: these are the values a
    // 2.006 s power and a follower at 54% health wrote before the rounding.
    const auto j = Parse(log::FormatEvent(log::Level::Info, "rule.resolved", kVersion, kStamp,
                                          {{"durationS", 2.006F}, {"healthPct", 0.5408737F}}));

    CHECK(j["durationS"] == 2.006);
    CHECK(j["healthPct"] == 0.541);
}

TEST_CASE("an id is one spelling: eight digits, upper case, 0x", "[logevent]")
{
    CHECK(log::Id(0xFF000DE0) == "0xFF000DE0");
    CHECK(log::Id(0xA2C94) == "0x000A2C94");
    CHECK(log::Id(0) == "0x00000000");
}

TEST_CASE("a party is an array of ids, not a joined string", "[logevent]")
{
    const std::vector<std::uint32_t> enemies{0x101, 0x102};
    const auto j = Parse(log::FormatEvent(log::Level::Info, "combat.entered", kVersion, kStamp,
                                          {{"allies", std::vector<std::uint32_t>{}}, {"enemies", enemies}}));

    REQUIRE(j["enemies"].is_array());
    CHECK(j["enemies"].size() == 2);
    CHECK(j["enemies"][0] == "0x00000101");
    CHECK(j["enemies"][1] == "0x00000102");

    CHECK(j["allies"].is_array());
    CHECK(j["allies"].empty());
}

TEST_CASE("fields keep the order they were written", "[logevent]")
{
    // The envelope first, then the event's own fields as the call site listed
    // them: a .jsonl read by eye should read like the prose line beside it.
    const auto line = log::FormatEvent(log::Level::Info, "rule.fired", kVersion, kStamp,
                                       {{"ruleIndex", 0}, {"ruleName", "emergency heal"}, {"action", "drink"}});

    CHECK(line.find("\"ruleIndex\"") < line.find("\"ruleName\""));
    CHECK(line.find("\"ruleName\"") < line.find("\"action\""));
    CHECK(line.find("\"event\"") < line.find("\"ruleIndex\""));
}

TEST_CASE("every level has a name, and reads back", "[logevent]")
{
    CHECK(log::ToString(log::Level::Debug) == std::string("debug"));
    CHECK(log::ToString(log::Level::Info) == std::string("info"));
    CHECK(log::ToString(log::Level::Warn) == std::string("warn"));
    CHECK(log::ToString(log::Level::Error) == std::string("error"));

    log::Level parsed{log::Level::Info};
    REQUIRE(log::ParseLevel("debug", parsed));
    CHECK(parsed == log::Level::Debug);

    // The ini is hand-edited, so case is not the player's problem.
    REQUIRE(log::ParseLevel("WARN", parsed));
    CHECK(parsed == log::Level::Warn);

    // Anything else leaves the caller's value alone, to fall back on.
    parsed = log::Level::Error;
    CHECK_FALSE(log::ParseLevel("verbose", parsed));
    CHECK_FALSE(log::ParseLevel("", parsed));
    CHECK(parsed == log::Level::Error);
}
