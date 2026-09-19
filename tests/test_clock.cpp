// The tactics clock: game hours into real seconds. What the game side
// hands it is two numbers off the calendar; what it makes of them is here.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Clock.h"

using Catch::Approx;
using ft::GameClock;

TEST_CASE("the first reading counts nothing, and each later one the hours since", "[clock]")
{
    GameClock clock;
    REQUIRE(clock.Seconds() == 0.0);
    REQUIRE(clock.Sample(12.0, 20.0) == 0.0);
    // Ten game minutes at timescale 20: thirty real seconds.
    REQUIRE(clock.Sample(12.0 + 10.0 / 60.0, 20.0) == Approx(30.0));
    // The same hour again: nothing passed.
    REQUIRE(clock.Sample(12.0 + 10.0 / 60.0, 20.0) == Approx(30.0));
    REQUIRE(clock.Seconds() == Approx(30.0));
}

TEST_CASE("the timescale is divided out at each step, so a second is a second of play", "[clock]")
{
    GameClock clock;
    clock.Sample(6.0, 20.0);
    // One game hour at 20 is three real minutes; at 1 it is an hour.
    REQUIRE(clock.Sample(7.0, 20.0) == Approx(180.0));
    REQUIRE(clock.Sample(8.0, 1.0) == Approx(180.0 + 3600.0));
    // A timescale of zero or less -- a console command, an unset global --
    // falls back to the game's default rather than dividing by it.
    REQUIRE(clock.Sample(9.0, 0.0) == Approx(180.0 + 3600.0 + 180.0));
    REQUIRE(clock.Sample(10.0, -5.0) == Approx(180.0 + 3600.0 + 360.0));
}

TEST_CASE("midnight is crossed when the hour goes down", "[clock]")
{
    GameClock clock;
    clock.Sample(23.5, 20.0);
    // 23:30 to 00:30 is one hour, not minus twenty-three.
    REQUIRE(clock.Sample(0.5, 20.0) == Approx(180.0));
}

TEST_CASE("what the hour alone cannot tell: a day slept, and an hour set back", "[clock]")
{
    // Known and accepted (Clock.h): the hour of day is the only reading,
    // so exactly a day is no time, and an hour gone backwards is read as
    // the next day's. A rule waits or fires a little early on a console
    // clock change; nothing worse.
    GameClock clock;
    clock.Sample(20.0, 20.0);
    REQUIRE(clock.Sample(20.0, 20.0) == 0.0);
    // 20:00 to 8:00 reads as twelve hours on, whether the player waited
    // through the night or set the hour back.
    REQUIRE(clock.Sample(8.0, 20.0) == Approx(12.0 * 180.0));
}
