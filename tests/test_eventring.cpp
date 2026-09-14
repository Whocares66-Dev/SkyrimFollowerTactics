// The last game events in memory, as the panel reads them: how many are
// kept, which go first, and how a reader asks for only what is new.

#include <catch2/catch_test_macros.hpp>

#include "core/EventRing.h"

#include <string>
#include <vector>

using namespace ft::log;

namespace
{

LoggedEvent Named(std::string name)
{
    LoggedEvent event;
    event.name = std::move(name);
    event.prose = "prose";
    event.fields = {{"ruleIndex", 2}};
    return event;
}

std::vector<std::uint64_t> Sequences(const std::vector<LoggedEvent> &events)
{
    std::vector<std::uint64_t> out;
    for (const auto &event : events)
        out.push_back(event.sequence);
    return out;
}

} // namespace

TEST_CASE("an empty ring has nothing to give", "[eventring]")
{
    const EventRing ring(3);
    CHECK(ring.Latest() == 0);
    CHECK(ring.Size() == 0);
    CHECK(ring.Since(0).empty());
}

TEST_CASE("the ring keeps the latest events, oldest dropped first", "[eventring]")
{
    EventRing ring(3);
    for (const char *name : {"a", "b", "c", "d", "e"})
        ring.Push(Named(name));

    CHECK(ring.Size() == 3);
    CHECK(ring.Latest() == 5);
    const auto held = ring.Since(0);
    CHECK(Sequences(held) == std::vector<std::uint64_t>{3, 4, 5});
    CHECK(held.front().name == "c");
    // What an event carried comes back whole.
    CHECK(held.front().prose == "prose");
    REQUIRE(held.front().fields.size() == 1);
    CHECK(std::string(held.front().fields.front().key()) == "ruleIndex");
}

TEST_CASE("a reader asks for what is newer than the last number it saw", "[eventring]")
{
    EventRing ring(3);
    for (const char *name : {"a", "b", "c", "d", "e"})
        ring.Push(Named(name));

    CHECK(Sequences(ring.Since(3)) == std::vector<std::uint64_t>{4, 5});
    CHECK(Sequences(ring.Since(4)) == std::vector<std::uint64_t>{5});
    CHECK(ring.Since(5).empty());
    CHECK(ring.Since(9).empty());
    // A reader behind what was dropped gets everything still held.
    CHECK(Sequences(ring.Since(1)) == std::vector<std::uint64_t>{3, 4, 5});
}

TEST_CASE("a ring holds at least one event", "[eventring]")
{
    EventRing ring(0);
    CHECK(ring.Push(Named("a")) == 1);
    CHECK(ring.Push(Named("b")) == 2);
    CHECK(Sequences(ring.Since(0)) == std::vector<std::uint64_t>{2});
}
