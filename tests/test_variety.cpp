// Variety in the AI's attack spells: the held Gumbel draw and the recency
// penalty (core/Variety.h).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/Variety.h"

#include <algorithm>
#include <cstdint>

using namespace ft;
using Catch::Approx;

namespace
{

// Which of two entries the engine's "best first" takes, over many fresh
// fights: the share entry A wins.
double ShareOfFirst(float a, float b, VarietySettings settings, int fights = 20000)
{
    Variety variety(12345, settings);
    int wins = 0;
    for (int i = 0; i < fights; ++i)
    {
        variety.Reset(0.0);
        const float scoreA = variety.Adjust(1, 101, a, 0.0).score;
        const float scoreB = variety.Adjust(2, 102, b, 0.0).score;
        wins += scoreA > scoreB ? 1 : 0;
    }
    return static_cast<double>(wins) / fights;
}

} // namespace

TEST_CASE("a score of zero or less is left alone, so the engine still never queues it", "[variety]")
{
    Variety variety(1);
    variety.Reset(0.0);
    CHECK(variety.Adjust(1, 101, 0.0f, 0.0).score == 0.0f);
    CHECK(variety.Adjust(2, 102, -1.0f, 0.0).score == -1.0f);
}

TEST_CASE("at temperature zero the engine's scores stand", "[variety]")
{
    VarietySettings settings;
    settings.temperature = 0.0;
    Variety variety(7, settings);
    variety.Reset(0.0);
    const auto varied = variety.Adjust(1, 101, 42.0f, 0.0);
    CHECK(varied.draw == Approx(1.0f));
    CHECK(varied.score == Approx(42.0f));
}

TEST_CASE("the pick is in proportion to score ^ (1 / T)", "[variety]")
{
    VarietySettings settings;
    settings.temperature = 1.0;
    // 3 against 1: three wins in four.
    CHECK(ShareOfFirst(3.0f, 1.0f, settings) == Approx(0.75).margin(0.015));
    // At T = 0.5 the ratio is squared: 9 against 1.
    settings.temperature = 0.5;
    CHECK(ShareOfFirst(3.0f, 1.0f, settings) == Approx(0.9).margin(0.015));
    // Equal scores, a coin toss.
    settings.temperature = 1.0;
    CHECK(ShareOfFirst(10.0f, 10.0f, settings) == Approx(0.5).margin(0.015));
}

TEST_CASE("the draw's factor stays within ten times either way", "[variety]")
{
    Variety variety(2024);
    double lowest = 1.0;
    double highest = 1.0;
    for (int i = 0; i < 20000; ++i)
    {
        variety.Reset(0.0);
        const double draw = variety.Adjust(1, 101, 1.0f, 0.0).draw;
        lowest = std::min(lowest, draw);
        highest = std::max(highest, draw);
    }
    CHECK(lowest >= 0.1 - 1e-6);
    CHECK(highest <= 10.0 + 1e-4);
    // And it reaches the cap: the tail is that long.
    CHECK(highest == Approx(10.0).margin(1e-4));
}

TEST_CASE("the draw is scale-free: every score times a constant picks the same", "[variety]")
{
    VarietySettings settings;
    CHECK(ShareOfFirst(3.0f, 1.0f, settings) == Approx(ShareOfFirst(300.0f, 100.0f, settings)).margin(1e-9));
}

TEST_CASE("a draw is held until its spell is cast, or it cannot be used, or the enemy changes", "[variety]")
{
    VarietySettings settings; // a cast of it releases it after the engine's 3 s
    Variety variety(99, settings);
    variety.Reset(0.0);
    constexpr std::uint32_t kBandit = 0x100;
    (void)variety.Adjust(2, 202, 10.0f, 0.0, kBandit); // another attack spell
    const auto first = variety.Adjust(1, 101, 10.0f, 0.0, kBandit);
    REQUIRE(first.fresh);

    SECTION("with nothing cast, the same draw however long: no timer")
    {
        const auto again = variety.Adjust(1, 101, 10.0f, 600.0, kBandit);
        CHECK_FALSE(again.fresh);
        CHECK(again.drawn == first.drawn);
    }
    SECTION("another spell cast does not take it: the chosen spell keeps its turn")
    {
        variety.NoteCast(202);
        CHECK(variety.Adjust(1, 101, 10.0f, 5.0, kBandit).drawn == first.drawn);
    }
    SECTION("its own cast inside the minimum hold waits for it")
    {
        variety.NoteCast(101);
        CHECK(variety.Adjust(1, 101, 10.0f, 2.5, kBandit).drawn == first.drawn);
        const auto later = variety.Adjust(1, 101, 10.0f, 3.0, kBandit);
        CHECK(later.fresh);
        CHECK(later.drawn != first.drawn);
    }
    SECTION("its own cast after the minimum hold draws anew at the next ask")
    {
        variety.NoteCast(101);
        CHECK(variety.Adjust(1, 101, 10.0f, 4.0, kBandit).fresh);
    }
    SECTION("another enemy draws anew")
    {
        CHECK(variety.Adjust(1, 101, 10.0f, 1.0, 0x200).fresh);
    }
    SECTION("unusable, the draw goes: released, or answered 0")
    {
        variety.Release(1);
        CHECK(variety.Adjust(1, 101, 10.0f, 1.0, kBandit).fresh);
        CHECK(variety.Adjust(1, 101, 0.0f, 2.0, kBandit).score == 0.0f);
        CHECK(variety.Adjust(1, 101, 10.0f, 2.0, kBandit).fresh);
    }
}

TEST_CASE("each hand's entry of a spell draws for itself; a cast counts against both", "[variety]")
{
    VarietySettings settings;
    settings.temperature = 0.0; // the penalty alone
    Variety variety(5, settings);
    variety.Reset(0.0);
    for (const auto &[entry, spell] : {std::pair{1, 101u}, std::pair{2, 101u}, std::pair{3, 102u}})
        (void)variety.Adjust(entry, spell, 10.0f, 0.0);
    variety.NoteCast(101);
    // Left and right entries of the same spell, both penalised.
    CHECK(variety.Adjust(1, 101, 10.0f, 1.0).recency == Approx(0.5f));
    CHECK(variety.Adjust(2, 101, 10.0f, 1.0).recency == Approx(0.5f));
    // Another spell, untouched.
    CHECK(variety.Adjust(3, 102, 10.0f, 1.0).recency == Approx(1.0f));
}

TEST_CASE("a spell's penalty is by attack casts ago, fading by half a cast", "[variety]")
{
    VarietySettings settings;
    settings.temperature = 0.0;
    Variety variety(5, settings);
    variety.Reset(0.0);
    (void)variety.Adjust(1, 101, 10.0f, 0.0);
    (void)variety.Adjust(2, 102, 10.0f, 0.0);
    const auto recency = [&](std::uint32_t spell) { return variety.Adjust(1, spell, 10.0f, 0.0).recency; };

    variety.NoteCast(101); // A
    CHECK(recency(101) == Approx(0.5f));
    variety.NoteCast(102); // A, then B: A one cast ago
    CHECK(recency(101) == Approx(0.75f));
    CHECK(recency(102) == Approx(0.5f));
    variety.NoteCast(101); // A, B, A: both casts of A count
    CHECK(recency(101) == Approx(0.5f * 0.875f));

    // A heal or a buff -- never scored here -- neither counts nor ages.
    variety.NoteCast(999);
    CHECK(recency(101) == Approx(0.5f * 0.875f));
    CHECK(recency(999) == Approx(1.0f));

    // Eight casts of B later, A is out of the memory, and whole again.
    for (int i = 0; i < 8; ++i)
        variety.NoteCast(102);
    CHECK(recency(101) == Approx(1.0f));
}

TEST_CASE("a new fight forgets the casts and draws afresh", "[variety]")
{
    VarietySettings settings;
    settings.temperature = 0.0;
    Variety variety(5, settings);
    variety.Reset(0.0);
    (void)variety.Adjust(1, 101, 10.0f, 0.0);
    variety.NoteCast(101);
    variety.Reset(2.0);
    const auto after = variety.Adjust(1, 101, 10.0f, 2.0);
    CHECK(after.fresh);
    CHECK(after.recency == Approx(1.0f));
}

TEST_CASE("a clock gone backwards starts the hold over", "[variety]")
{
    Variety variety(3);
    variety.Reset(100.0);
    (void)variety.Adjust(1, 101, 10.0f, 100.0);
    CHECK(variety.Adjust(1, 101, 10.0f, 5.0).fresh);
}
