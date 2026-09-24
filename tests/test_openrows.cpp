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
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), false));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 1), false));

    // Rule 0 moved to 1: its drawer goes with it, the closed one comes back.
    rows.Move(kLydia, Moment::Combat, 0, 1);
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), false));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 1), false));
    // Both open: both stay open.
    rows.Open(OpenRows::Key(kLydia, Moment::Combat, 0));
    rows.Move(kLydia, Moment::Combat, 0, 1);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), false));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 1), false));
    // The other actor's are untouched.
    REQUIRE(rows.IsOpen(OpenRows::Key(kJenassa, Moment::Combat, 0), false));

    rows.Close(OpenRows::Key(kLydia, Moment::Combat, 0));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), false));
}

TEST_CASE("a rule removed takes its drawer, and the drawers after it move up", "[openrows]")
{
    constexpr ActorId kLydia = 0xA2C94;
    OpenRows rows;
    for (const std::size_t i : {0u, 2u, 3u})
        rows.Open(OpenRows::Key(kLydia, Moment::Combat, i));
    // Of four rules, the second (closed) removed: 2 and 3 become 1 and 2.
    rows.Remove(kLydia, Moment::Combat, 1, 4);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), false));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 1), false));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 2), false));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 3), false));
    // The first removed, open: gone, the rest up.
    rows.Remove(kLydia, Moment::Combat, 0, 3);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), false));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 1), false));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 2), false));
    // The last removed: nothing to move.
    rows.Remove(kLydia, Moment::Combat, 1, 2);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), false));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 1), false));
    rows.Clear();
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), false));
}

TEST_CASE("an actor's two lists keep their drawers apart", "[openrows]")
{
    constexpr ActorId kLydia = 0xA2C94;
    OpenRows rows;
    // Opening the combat list's first rule leaves the idle list's closed.
    rows.Open(OpenRows::Key(kLydia, Moment::Combat, 0));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), false));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Idle, 0), false));

    // Deleting the idle list's first rule leaves the combat drawer open.
    rows.Open(OpenRows::Key(kLydia, Moment::Idle, 1));
    rows.Remove(kLydia, Moment::Idle, 0, 2);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), false));
    // The idle list's own drawer moved up, as it should.
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Idle, 0), false));

    // Reordering one list does not touch the other.
    rows.Move(kLydia, Moment::Idle, 0, 1);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), false));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Idle, 1), false));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Idle, 0), false));
}

TEST_CASE("an untouched drawer is whatever its caller says, a clicked one is remembered", "[openrows]")
{
    constexpr ActorId kLydia = 0xA2C94;
    const std::string key = OpenRows::Key(kLydia, Moment::Combat, 0);
    OpenRows rows;

    // Nothing clicked: each caller gets its own answer. A rule's actions
    // are drawn expanded and a sheet's extra rows folded, off one book.
    REQUIRE(rows.IsOpen(key, true));
    REQUIRE_FALSE(rows.IsOpen(key, false));

    // Clicked shut: shut, even where the default is open. This is the case
    // the set of open keys could not hold -- absence WAS closed, so a
    // default of open could not be overridden.
    rows.Close(key);
    REQUIRE_FALSE(rows.IsOpen(key, true));
    REQUIRE_FALSE(rows.IsOpen(key, false));

    // And clicked open: open, even where the default is closed.
    rows.Open(key);
    REQUIRE(rows.IsOpen(key, true));
    REQUIRE(rows.IsOpen(key, false));
}

TEST_CASE("a drawer nobody clicked stays unclicked through a move and a removal", "[openrows]")
{
    constexpr ActorId kLydia = 0xA2C94;
    OpenRows rows;

    // Rule 1 is clicked shut; rules 0 and 2 are untouched.
    rows.Close(OpenRows::Key(kLydia, Moment::Combat, 1));

    // Moved to 0, the shut one is still shut and 1 is untouched again --
    // it must not come back as CLOSED, or a rule nobody touched would be
    // pinned to today's default and stop following it.
    rows.Move(kLydia, Moment::Combat, 1, 0);
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), true));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 1), true));

    // Removing rule 0 moves the untouched 1 down onto 0, still untouched.
    rows.Remove(kLydia, Moment::Combat, 0, 3);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), true));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), false));
}

TEST_CASE("a rule dragged over several carries its drawer, and the ones between shift", "[openrows][drag]")
{
    constexpr ActorId kLydia = 0xA2C94;
    OpenRows rows;
    // Of five rules, 1 is open and 3 clicked shut; the others untouched.
    rows.Open(OpenRows::Key(kLydia, Moment::Combat, 1));
    rows.Close(OpenRows::Key(kLydia, Moment::Combat, 3));

    // 1 dragged down to 3: 2 and 3 move up to 1 and 2.
    rows.Move(kLydia, Moment::Combat, 1, 3);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 3), false));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 2), true)); // the shut one, moved up
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 1), true));       // untouched, moved up
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 1), false));

    // And back up to 0: 0, 1 and 2 move down a place.
    rows.Move(kLydia, Moment::Combat, 3, 0);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), false));
    REQUIRE_FALSE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 3), true));
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 4), true)); // outside the span: untouched

    // Onto itself: nothing.
    rows.Move(kLydia, Moment::Combat, 0, 0);
    REQUIRE(rows.IsOpen(OpenRows::Key(kLydia, Moment::Combat, 0), false));
}

TEST_CASE("a drop lands where the line was drawn, and the list follows", "[openrows][drag]")
{
    // Dropped above itself or just below itself: where it was.
    REQUIRE(DroppedAt(2, 2) == 2);
    REQUIRE(DroppedAt(2, 3) == 2);
    // Above an earlier row: that row's place. Below a later one: one less,
    // its own place having left the list.
    REQUIRE(DroppedAt(2, 0) == 0);
    REQUIRE(DroppedAt(0, 5) == 4);
    REQUIRE(DroppedAt(1, 4) == 3);

    std::vector<char> list{'a', 'b', 'c', 'd', 'e'};
    MoveItem(list, 1, DroppedAt(1, 4)); // b dropped between d and e
    REQUIRE(list == std::vector<char>{'a', 'c', 'd', 'b', 'e'});
    MoveItem(list, 4, DroppedAt(4, 0)); // e dropped above all
    REQUIRE(list == std::vector<char>{'e', 'a', 'c', 'd', 'b'});
    MoveItem(list, 0, 1); // one step: a swap
    REQUIRE(list == std::vector<char>{'a', 'e', 'c', 'd', 'b'});
    MoveItem(list, 2, 9); // out of range: nothing
    REQUIRE(list == std::vector<char>{'a', 'e', 'c', 'd', 'b'});
}
