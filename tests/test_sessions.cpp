// A session's files without a disk: when it began, read back from its first
// line; the name its pair is archived under; which archived pairs go; and the
// ceiling on one file.

#include <catch2/catch_test_macros.hpp>

#include "core/LogEvent.h"
#include "core/Sessions.h"

#include <string>
#include <vector>

using namespace ft::log;

namespace
{

constexpr UtcTime kStart{2026, 9, 14, 19, 53, 2};
constexpr UtcTime kEnd{2026, 9, 14, 20, 10, 3};

} // namespace

TEST_CASE("a time is read as the envelope and the banner write it", "[sessions]")
{
    CHECK(ParseIsoTime("2026-09-14T19:53:02") == kStart);
    CHECK(ParseIsoTime("2026-09-14T19:53:02Z") == kStart);
    CHECK(ParseIsoTime("2026-09-14T19:53:02.400Z") == kStart);

    CHECK_FALSE(ParseIsoTime(""));
    CHECK_FALSE(ParseIsoTime("2026-09-14 19:53:02"));
    CHECK_FALSE(ParseIsoTime("2026-09-14T19:53"));
    CHECK_FALSE(ParseIsoTime("20x6-09-14T19:53:02Z"));
    CHECK_FALSE(ParseIsoTime("2026-13-14T19:53:02Z"));
    CHECK_FALSE(ParseIsoTime("2026-09-14T19:53:02Z and more"));
    CHECK_FALSE(ParseIsoTime("2026-09/14T19:53:02Z"));
    CHECK_FALSE(ParseIsoTime("2026-00-14T19:53:02Z"));
    CHECK_FALSE(ParseIsoTime("2026-09-14T24:00:00Z"));
}

TEST_CASE("a session's start is read back from either file's first line", "[sessions]")
{
    const std::string started = FormatEvent(Level::Info, "session.started", "0.1.0", "2026-09-14T19:53:02.400Z", {});
    CHECK(SessionStart(started) == kStart);
    // The sink ends lines with \r\n, and getline leaves the \r.
    CHECK(SessionStart(started + "\r") == kStart);
    CHECK(SessionStart("[12:53:02.400] [ 1234] [I] [plugin]   FollowerTactics v0.1.0 -- session began "
                       "2026-09-14T19:53:02.400Z\r") == kStart);

    // A log from before sessions were dated, another event first, a broken
    // line, nothing at all.
    CHECK_FALSE(SessionStart("[12:53:02.400] [ 1234] [I] [plugin]   FollowerTactics v0.1.0"));
    CHECK_FALSE(SessionStart(FormatEvent(Level::Info, "rule.fired", "0.1.0", "2026-09-14T19:53:02.400Z", {})));
    CHECK_FALSE(SessionStart(R"({"event":"session.started","ts":7})"));
    CHECK_FALSE(SessionStart(R"({"event":5,"ts":"2026-09-14T19:53:02Z"})"));
    CHECK_FALSE(SessionStart(R"({"event":"session.started"})"));
    CHECK_FALSE(SessionStart(R"({"ts":"2026-09-14T19:53:02Z"})"));
    CHECK_FALSE(SessionStart("{not json"));
    CHECK_FALSE(SessionStart(""));
}

TEST_CASE("an archived session is named for its start and end", "[sessions]")
{
    CHECK(Stamp(kStart) == "2026-09-14-19-53-02");
    CHECK(ArchiveStem(kStart, kEnd) == "FollowerTactics-2026-09-14-19-53-02_2026-09-14-20-10-03");
    CHECK(ArchiveStem(std::nullopt, kEnd) == "FollowerTactics-_2026-09-14-20-10-03");

    CHECK(ArchiveEnd(ArchiveStem(kStart, kEnd)) == kEnd);
    CHECK(ArchiveEnd(ArchiveStem(std::nullopt, kEnd)) == kEnd);

    // Only a name this made has an end, so nothing else in the folder is
    // ever pruned.
    CHECK_FALSE(ArchiveEnd("crash-2026-09-14-06-39-15"));
    CHECK_FALSE(ArchiveEnd("FollowerTactics"));
    CHECK_FALSE(ArchiveEnd("FollowerTactics-2026-09-14-19-53-02"));
    CHECK_FALSE(ArchiveEnd("FollowerTactics-notes_2026-09-14-20-10-03"));
    CHECK_FALSE(ArchiveEnd("FollowerTactics-2026-09-14-19-53-02_later"));
}

TEST_CASE("the archive keeps the latest sessions", "[sessions]")
{
    const auto endedAt = [](int hour) { return ArchiveStem(kStart, UtcTime{2026, 9, 14, hour, 0, 0}); };

    CHECK(StemsToDelete({endedAt(20)}, 2).empty());
    CHECK(StemsToDelete({endedAt(20), endedAt(21)}, 2).empty());

    // Out of order, a pair with no start, a name this did not make, and one
    // stem listed twice (its .log and its .events.jsonl).
    const std::string undated = ArchiveStem(std::nullopt, UtcTime{2026, 9, 14, 18, 0, 0});
    const auto doomed =
        StemsToDelete({endedAt(22), "crash-2026-09-14-06-39-15", endedAt(20), undated, endedAt(21), endedAt(20)}, 2);
    CHECK(doomed == std::vector<std::string>{undated, endedAt(20)});
}

TEST_CASE("a file takes lines to its ceiling, says so once, then nothing", "[sessions]")
{
    CHECK(RoomFor(0, 10, 100) == Room::Write);
    CHECK(RoomFor(90, 10, 100) == Room::Write);
    CHECK(RoomFor(95, 10, 100) == Room::Last);
    CHECK(RoomFor(100, 10, 100) == Room::Last);
    CHECK(RoomFor(110, 10, 100) == Room::None);
}
