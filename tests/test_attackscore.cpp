// An attack spell per second of a follower's time, magicka counted as time
// (core/AttackScore.h).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/AttackScore.h"

using namespace ft;
using Catch::Approx;

namespace
{

// A pool of 200 coming back at 2 points a second: half a second a point.
MagickaPool Pool(float current)
{
    return {current, 200.0f, 2.0f};
}

} // namespace

TEST_CASE("magicka costs nothing with the pool full, and a point's regeneration time with it empty", "[attackscore]")
{
    CHECK(MagickaPrice(Pool(200.0f)) == Approx(0.0));
    CHECK(MagickaPrice(Pool(0.0f)) == Approx(0.5));
    // Squared between: at half, a quarter of the way.
    CHECK(MagickaPrice(Pool(100.0f)) == Approx(0.125));
    CHECK(MagickaPrice(Pool(150.0f)) == Approx(0.03125));
}

TEST_CASE("a pool that does not come back prices a point at the cap, and no pool at nothing", "[attackscore]")
{
    CHECK(MagickaPrice({0.0f, 200.0f, 0.0f}) == Approx(kMaxSecondsPerPoint));
    CHECK(MagickaPrice({0.0f, 200.0f, 0.01f}) == Approx(kMaxSecondsPerPoint));
    CHECK(MagickaPrice({0.0f, 0.0f, 2.0f}) == Approx(0.0));
    // Over the maximum (a Fortify ending) reads as full.
    CHECK(MagickaPrice({250.0f, 200.0f, 2.0f}) == Approx(0.0));
}

TEST_CASE("the AI holds an attack spell half a second to a second and a half by its style", "[attackscore]")
{
    CHECK(HoldSeconds(0.0) == Approx(0.5));
    CHECK(HoldSeconds(0.5) == Approx(1.0));
    CHECK(HoldSeconds(1.0) == Approx(1.5));
    CHECK(HoldSeconds(3.0) == Approx(1.5));
}

TEST_CASE("a released spell's cycle is its charge and the hold; a stream's is the scoring duration", "[attackscore]")
{
    const SpellCycle released = ReleasedCycle(0.5, 1.0, 41.0);
    CHECK(released.seconds == Approx(1.5));
    CHECK(released.magicka == Approx(41.0));
    const SpellCycle stream = StreamCycle(3.0, 14.0);
    CHECK(stream.seconds == Approx(3.0));
    CHECK(stream.magicka == Approx(42.0));
    // Nothing to charge and no hold: the floor, not a division by zero.
    CHECK(ReleasedCycle(0.0, 0.0, 10.0).seconds == Approx(0.1));
}

TEST_CASE("the dear spell wins on a full pool and the cheap one on a low one", "[attackscore]")
{
    // Firebolt: 25 a cast for 41. Incinerate: 60 a cast for 110. Both
    // charge half a second and are held a second.
    const SpellCycle firebolt = ReleasedCycle(0.5, 1.0, 41.0);
    const SpellCycle incinerate = ReleasedCycle(0.5, 1.0, 110.0);
    const auto score = [&](float magicka, double damage, const SpellCycle &cycle) {
        return PerSecond(damage, cycle, MagickaPrice(Pool(magicka)));
    };
    // Full: damage per second alone.
    CHECK(score(200.0f, 25.0, firebolt) == Approx(25.0 / 1.5));
    CHECK(score(200.0f, 60.0, incinerate) > score(200.0f, 25.0, firebolt));
    // A quarter left: magicka is dear, and the cheap spell does more with it.
    CHECK(score(50.0f, 25.0, firebolt) > score(50.0f, 60.0, incinerate));
}

TEST_CASE("a scroll waits for the pool to run low: none of its score full, all of it empty", "[attackscore]")
{
    CHECK(ScrollReserve(Pool(200.0f)) == Approx(0.0));
    CHECK(ScrollReserve(Pool(100.0f)) == Approx(0.25));
    CHECK(ScrollReserve(Pool(0.0f)) == Approx(1.0));
    // One with no magicka at all has nothing to save it for.
    CHECK(ScrollReserve({0.0f, 0.0f, 0.0f}) == Approx(1.0));
    // A quarter left: the scroll's 60 a cast, free, against Firebolt's 25 for 41.
    const double scroll = PerSecond(60.0, ReleasedCycle(0.5, 1.0, 0.0), 0.0) * ScrollReserve(Pool(50.0f));
    const double firebolt = PerSecond(25.0, ReleasedCycle(0.5, 1.0, 41.0), MagickaPrice(Pool(50.0f)));
    CHECK(scroll > firebolt);
}

TEST_CASE("a scroll or a staff costs no magicka, so the pool does not touch its score", "[attackscore]")
{
    const SpellCycle scroll = ReleasedCycle(0.5, 1.0, 0.0);
    CHECK(PerSecond(30.0, scroll, MagickaPrice(Pool(0.0f))) == Approx(20.0));
    CHECK(PerSecond(30.0, scroll, MagickaPrice(Pool(200.0f))) == Approx(20.0));
}
