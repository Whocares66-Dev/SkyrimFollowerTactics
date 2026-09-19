// Whether an effect does anything for the actor, and whether it is a buff.

#include <catch2/catch_test_macros.hpp>

#include "core/Effects.h"

using namespace ft;

TEST_CASE("a skill modifier applies only where a perk reads it; anything else applies", "[effects]")
{
    EffectShape fortifyOneHanded;
    fortifyOneHanded.valueModifier = true;
    fortifyOneHanded.peakValue = true;
    fortifyOneHanded.skillModifier = true;
    fortifyOneHanded.duration = 60.0f;
    REQUIRE_FALSE(EffectApplies(fortifyOneHanded, false, false));
    REQUIRE(EffectApplies(fortifyOneHanded, true, false));
    EffectShape power = fortifyOneHanded;
    power.skillModifier = false;
    power.skillPower = true;
    REQUIRE_FALSE(EffectApplies(power, true, false));
    REQUIRE(EffectApplies(power, false, true));
    EffectShape resist;
    resist.valueModifier = true;
    resist.peakValue = true;
    resist.duration = 60.0f;
    REQUIRE(EffectApplies(resist, false, false));
    EffectShape invisibility;
    invisibility.duration = 30.0f;
    REQUIRE(EffectApplies(invisibility, false, false));
}

TEST_CASE("a buff lasts, helps, is a peak value the actor reads, and is not waterbreathing", "[effects]")
{
    EffectShape resist;
    resist.valueModifier = true;
    resist.peakValue = true;
    resist.duration = 60.0f;
    REQUIRE(IsBuff(resist, false, false));
    EffectShape instant = resist;
    instant.duration = 0.0f;
    REQUIRE_FALSE(IsBuff(instant, false, false));
    EffectShape harmful = resist;
    harmful.harmful = true;
    REQUIRE_FALSE(IsBuff(harmful, false, false));
    EffectShape restore = resist;
    restore.peakValue = false;
    REQUIRE_FALSE(IsBuff(restore, false, false));
    EffectShape water = resist;
    water.waterbreathing = true;
    REQUIRE_FALSE(IsBuff(water, false, false));
    EffectShape skill = resist;
    skill.skillModifier = true;
    REQUIRE_FALSE(IsBuff(skill, false, false));
    REQUIRE(IsBuff(skill, true, false));
}
