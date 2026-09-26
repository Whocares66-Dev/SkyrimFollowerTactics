// A combat style tuned on the Combat Style tab: the steps, the ranges and
// the sums the game puts on a follower's copy of their style.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/CombatStyle.h"

using namespace ft;

TEST_CASE("a chance moves by hundredths up to 1, a score by tenths up to 10", "[combatstyle]")
{
    REQUIRE(StyleMax(StyleField::Offensive) == 1.0f);
    REQUIRE(StyleStep(StyleField::Offensive) == Catch::Approx(0.01f));
    REQUIRE(StyleMax(StyleField::MagicScore) == 10.0f);
    REQUIRE(StyleStep(StyleField::MagicScore) == Catch::Approx(0.1f));
    // The melee attack multipliers are on the scores' scale, the movement
    // ones on the chances'.
    REQUIRE(StyleMax(StyleField::BashPowerAttacking) == 10.0f);
    REQUIRE(StyleMax(StyleField::Strafe) == 1.0f);
}

TEST_CASE("a plus or minus snaps to its field's step", "[combatstyle]")
{
    REQUIRE(SnapStyleDelta(StyleField::Offensive, 0.30000001f) == 0.3f);
    REQUIRE(SnapStyleDelta(StyleField::Offensive, 0.004f) == 0.0f);
    REQUIRE(SnapStyleDelta(StyleField::Offensive, -0.126f) == -0.13f);
    REQUIRE(SnapStyleDelta(StyleField::MagicScore, 1.26f) == 1.3f);
    REQUIRE(SnapStyleDelta(StyleField::MagicScore, -0.04f) == 0.0f);
}

TEST_CASE("the sum stays on its scale, and a base past the top keeps its own", "[combatstyle]")
{
    REQUIRE(AdjustedStyleValue(StyleField::Offensive, 0.5f, 0.25f) == 0.75f);
    REQUIRE(AdjustedStyleValue(StyleField::Offensive, 0.5f, 0.75f) == 1.0f);
    REQUIRE(AdjustedStyleValue(StyleField::Offensive, 0.5f, -0.75f) == 0.0f);
    REQUIRE(AdjustedStyleValue(StyleField::MagicScore, 4.05f, 5.95f) == Catch::Approx(10.0f));
    REQUIRE(AdjustedStyleValue(StyleField::Offensive, 1.5f, 0.0f) == 1.5f);
    REQUIRE(AdjustedStyleValue(StyleField::Offensive, 1.5f, 0.5f) == 1.5f);
}

TEST_CASE("the slider runs from the base down to 0 and up to the top", "[combatstyle]")
{
    const auto [lo, hi] = StyleDeltaRange(StyleField::MagicScore, 4.05f);
    REQUIRE(lo == Catch::Approx(-4.05f));
    REQUIRE(hi == Catch::Approx(5.95f));
    const auto [lo1, hi1] = StyleDeltaRange(StyleField::Offensive, 1.5f);
    REQUIRE(lo1 == Catch::Approx(-1.5f));
    REQUIRE(hi1 == 0.0f);
}

TEST_CASE("every field has its own wire name, and reads back from it", "[combatstyle]")
{
    for (std::size_t i = 0; i < kStyleFields; ++i)
    {
        const auto field = static_cast<StyleField>(i);
        INFO(WireName(field));
        REQUIRE_FALSE(WireName(field).empty());
        REQUIRE(StyleFieldFromWireName(WireName(field)) == field);
    }
    REQUIRE_FALSE(StyleFieldFromWireName("avoid-threat").has_value());
}

TEST_CASE("no plus or minus anywhere, and no word on a switch, is no tuning", "[combatstyle]")
{
    StyleAdjustments adjustments;
    REQUIRE_FALSE(AnyStyleDelta(adjustments.deltas));
    REQUIRE_FALSE(AnyStyleAdjustment(adjustments));
    adjustments.deltas[static_cast<std::size_t>(StyleField::Strafe)] = -0.01f;
    REQUIRE(AnyStyleDelta(adjustments.deltas));
    REQUIRE(AnyStyleAdjustment(adjustments));
    adjustments.deltas = {};
    adjustments.switches[static_cast<std::size_t>(StyleSwitch::Flanking)] = false;
    REQUIRE(AnyStyleAdjustment(adjustments));
}

TEST_CASE("a switch turns its flag over, and back to the record's is no word", "[combatstyle]")
{
    const auto at = [](StyleSwitch which) { return static_cast<std::size_t>(which); };
    StyleAdjustments adjustments;
    // A record that forbids dual wielding: on is a word, and off again none.
    REQUIRE_FALSE(SwitchOn(adjustments, StyleSwitch::DualWield, false));
    adjustments.switches[at(StyleSwitch::DualWield)] = ToggledSwitch(adjustments, StyleSwitch::DualWield, false);
    REQUIRE(adjustments.switches[at(StyleSwitch::DualWield)] == true);
    REQUIRE(SwitchOn(adjustments, StyleSwitch::DualWield, false));
    adjustments.switches[at(StyleSwitch::DualWield)] = ToggledSwitch(adjustments, StyleSwitch::DualWield, false);
    REQUIRE_FALSE(adjustments.switches[at(StyleSwitch::DualWield)].has_value());
    // A record that flanks: off is the word, and the other switch is left
    // alone.
    adjustments.switches[at(StyleSwitch::Flanking)] = ToggledSwitch(adjustments, StyleSwitch::Flanking, true);
    REQUIRE(adjustments.switches[at(StyleSwitch::Flanking)] == false);
    REQUIRE_FALSE(SwitchOn(adjustments, StyleSwitch::Flanking, true));
    REQUIRE_FALSE(adjustments.switches[at(StyleSwitch::DualWield)].has_value());
    // The word stands whatever the record says: a record changed under it
    // keeps the player's choice.
    REQUIRE_FALSE(SwitchOn(adjustments, StyleSwitch::Flanking, false));
}

TEST_CASE("every switch has its own wire name, apart from the fields'", "[combatstyle]")
{
    for (std::size_t i = 0; i < kStyleSwitches; ++i)
    {
        const auto which = static_cast<StyleSwitch>(i);
        INFO(WireName(which));
        REQUIRE(StyleSwitchFromWireName(WireName(which)) == which);
        REQUIRE_FALSE(StyleFieldFromWireName(WireName(which)).has_value());
    }
}
