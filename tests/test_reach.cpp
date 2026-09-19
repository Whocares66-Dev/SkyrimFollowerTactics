// The reach measure: centre to centre, flat past a height, less the radii.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Reach.h"

using namespace ft;
using Catch::Approx;

namespace
{

// A human-sized actor at (x, y, z): 64 units tall, 20 across the y axis.
Body At(float x, float y, float z)
{
    Body body;
    body.x = x;
    body.y = y;
    body.z = z;
    body.minY = -20.0f;
    body.maxY = 20.0f;
    body.minZ = 0.0f;
    body.maxZ = 128.0f;
    return body;
}

} // namespace

TEST_CASE("a body's radius is its box's far y edge at its scale, or 16 with no box", "[reach]")
{
    Body body = At(0, 0, 0);
    REQUIRE(BodyRadius(body) == 20.0f);
    body.scale = 1.5f;
    REQUIRE(BodyRadius(body) == 30.0f);
    Body empty;
    REQUIRE(BodyRadius(empty) == kEmptyBoxRadius);
    empty.scale = 3.0f;
    REQUIRE(BodyRadius(empty) == kEmptyBoxRadius);
}

TEST_CASE("on level ground: centre to centre, less both radii", "[reach]")
{
    REQUIRE(ReachDistance(At(0, 0, 0), At(100, 0, 0)) == Approx(60.0f));
    REQUIRE(ReachDistance(At(0, 0, 0), At(30, 40, 0)) == Approx(10.0f));
    // Closer than the bodies: below zero, as the engine has it.
    REQUIRE(ReachDistance(At(0, 0, 0), At(10, 0, 0)) == Approx(-30.0f));
}

TEST_CASE("past 48 units of height, flat where the ends overlap, full where they do not", "[reach]")
{
    // 47.9 apart: the full distance.
    const float full = ReachDistance(At(0, 0, 0), At(100, 0, 47.9f));
    REQUIRE(full == Approx(std::sqrt(100.0f * 100.0f + 47.9f * 47.9f) - 40.0f));
    // 48 apart, the attacker's feet within the target's height: flat.
    REQUIRE(ReachDistance(At(0, 0, 0), At(100, 0, 48.0f)) == Approx(60.0f));
    REQUIRE(ReachDistance(At(0, 0, 0), At(100, 0, -48.0f)) == Approx(60.0f));
    // The attacker's head exactly at the target's feet: within, flat.
    REQUIRE(ReachDistance(At(0, 0, 0), At(100, 0, 128.0f)) == Approx(60.0f));
    // Above the target's head altogether: neither end within, full.
    const float above = ReachDistance(At(0, 0, 200.0f), At(100, 0, 0));
    REQUIRE(above == Approx(std::sqrt(100.0f * 100.0f + 200.0f * 200.0f) - 40.0f));
    // The target's bounds are what the ends are held against, unscaled.
    Body tall = At(100, 0, 0);
    tall.maxZ = 300.0f;
    REQUIRE(ReachDistance(At(0, 0, 200.0f), tall) == Approx(60.0f));
}
