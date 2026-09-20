// The panel's open drawers, following the rules they belong to.

#include <catch2/catch_test_macros.hpp>

#include "core/OpenRows.h"

using namespace ft;

TEST_CASE("a drawer follows its rule through a move, and the other way too", "[openrows]")
{
    constexpr ActorId kLydia = 0xA2C94;
    constexpr ActorId kJenassa = 0x1348A;
    OpenRows rows;
    rows.Open(OpenRows::Key(kLydia, Moment::Combat, 0));
    rows.Open(OpenRows::Key(kJenassa, Moment::Combat, 0));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0)));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 1)));

    // Rule 0 moved to 1: its drawer goes with it, the closed one comes back.
    rows.Move(kLydia, Moment::Combat, 0, 1);
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0)));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 1)));
    // Both open: both stay open.
    rows.Open(OpenRows::Key(kLydia, Moment::Combat, 0));
    rows.Move(kLydia, Moment::Combat, 0, 1);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0)));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 1)));
    // The other actor's are untouched.
    REQUIRE(rows.IsOpen(OpenRows::Key(kJenassa, Moment::Combat, 0)));

    rows.Close(OpenRows::Key(kLydia, Moment::Combat, 0));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0)));
}

TEST_CASE("a rule removed takes its drawer, and the drawers after it move up", "[openrows]")
{
    constexpr ActorId kLydia = 0xA2C94;
    OpenRows rows;
    for (const std::size_t i : {0u, 2u, 3u})
        rows.Open(OpenRows::Key(kLydia, Moment::Combat, i));
    // Of four rules, the second (closed) removed: 2 and 3 become 1 and 2.
    rows.Remove(kLydia, Moment::Combat, 1, 4);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0)));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 1)));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 2)));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 3)));
    // The first removed, open: gone, the rest up.
    rows.Remove(kLydia, Moment::Combat, 0, 3);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0)));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 1)));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 2)));
    // The last removed: nothing to move.
    rows.Remove(kLydia, Moment::Combat, 1, 2);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0)));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 1)));
    rows.Clear();
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0)));
}

TEST_CASE("an actor's two lists keep their drawers apart", "[openrows]")
{
    constexpr ActorId kLydia = 0xA2C94;
    OpenRows rows;
    // Opening the combat list's first rule leaves the idle list's closed.
    rows.Open(OpenRows::Key(kLydia, Moment::Combat, 0));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0)));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Idle, 0)));

    // Deleting the idle list's first rule leaves the combat drawer open.
    rows.Open(OpenRows::Key(kLydia, Moment::Idle, 1));
    rows.Remove(kLydia, Moment::Idle, 0, 2);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0)));
    // The idle list's own drawer moved up, as it should.
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Idle, 0)));

    // Reordering one list does not touch the other.
    rows.Move(kLydia, Moment::Idle, 0, 1);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0)));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Idle, 1)));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Idle, 0)));
}
