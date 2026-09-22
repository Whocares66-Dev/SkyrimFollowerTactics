#include "progression/core/Levelling.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using Catch::Approx;

TEST_CASE("a level costs what the player's next level costs", "[levelling]")
{
    const fp::Rules r; // Skyrim.esm's 75 + 25 x level
    CHECK(fp::LevelThreshold(r, 1) == 100.0);
    CHECK(fp::LevelThreshold(r, 30) == 825.0);
    CHECK(fp::XpToReach(r, 1) == 0.0);
    CHECK(fp::XpToReach(r, 10) == 1800.0);
    CHECK(fp::XpToReach(r, 30) == 13050.0);
    CHECK(fp::LevelFor(r, 0.0) == 1);
    CHECK(fp::LevelFor(r, 13049.0) == 29);
    CHECK(fp::LevelFor(r, 13050.0) == 30);
}

TEST_CASE("the skill formulas are the engine's, over whatever values the game has", "[levelling]")
{
    fp::Rules r;
    const fp::SkillUsage oneHanded{6.3, 0.0, 0.3, 2.0};
    CHECK(fp::SkillXp(oneHanded, 10.0) == Approx(63.0));
    CHECK(fp::SkillThreshold(r, oneHanded, 15) == Approx(0.3 * std::pow(15.0, 1.95) + 2.0));
    CHECK(fp::SkillThreshold(r, oneHanded, 100) == 0.0); // the cap
    CHECK(fp::XpForSkillLevel(r, 81) == 81.0);

    // A mod that changes a setting changes the answer.
    r.skillUseCurve = 1.5;
    r.xpPerSkillRank = 2.0;
    CHECK(fp::SkillThreshold(r, oneHanded, 15) == Approx(0.3 * std::pow(15.0, 1.5) + 2.0));
    CHECK(fp::XpForSkillLevel(r, 81) == 162.0);
    r.levelUpBase = 100.0;
    CHECK(fp::LevelThreshold(r, 1) == 125.0);
}
