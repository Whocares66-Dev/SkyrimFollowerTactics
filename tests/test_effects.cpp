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

TEST_CASE("a consumable's effects: every one, by name, the bane beside the boon", "[effects]")
{
    using Seen = ConsumableEffectSeen;
    EffectShape fortify;
    fortify.valueModifier = true;
    fortify.peakValue = true;
    fortify.duration = 60.0f;
    EffectShape restore;
    EffectShape damage;
    damage.harmful = true;
    damage.duration = 10.0f;

    // Sleeping Tree Sap: a boon and a bane, both kept, in the item's order.
    const std::vector<Seen> mixed{{"Fortify Health", 100.0f, 60.0f, fortify}, {"Slow", 50.0f, 10.0f, damage}};
    const auto both = ConsumableEffectsOf(mixed, ConsumableKind::Potion, false, false);
    REQUIRE(both.size() == 2);
    REQUIRE(both[0].name == "Fortify Health");
    REQUIRE(both[0].buff);
    REQUIRE_FALSE(both[0].harmful);
    REQUIRE(both[1].name == "Slow");
    REQUIRE(both[1].harmful);
    REQUIRE_FALSE(both[1].buff);

    // A restore lasts no time and is no buff, but is still an effect.
    const std::vector<Seen> healing{{"Restore Health", 50.0f, 0.0f, restore}};
    const auto one = ConsumableEffectsOf(healing, ConsumableKind::Potion, false, false);
    REQUIRE(one.size() == 1);
    REQUIRE_FALSE(one[0].buff);

    // A nameless effect, and one no follower can use, are left out.
    EffectShape disease;
    const std::vector<Seen> useless{{"", 10.0f, 0.0f, restore}, {"Cure Disease", 0.0f, 0.0f, disease}};
    REQUIRE(ConsumableEffectsOf(useless, ConsumableKind::Potion, false, false).empty());
}

TEST_CASE("an ingredient gives its first effect and no other, even when that one is left out", "[effects]")
{
    using Seen = ConsumableEffectSeen;
    EffectShape plain;
    const std::vector<Seen> two{{"Restore Health", 5.0f, 0.0f, plain}, {"Fortify Health", 20.0f, 60.0f, plain}};
    const auto eaten = ConsumableEffectsOf(two, ConsumableKind::Ingredient, false, false);
    REQUIRE(eaten.size() == 1);
    REQUIRE(eaten[0].name == "Restore Health");
    // Eaten as food, every effect counts.
    REQUIRE(ConsumableEffectsOf(two, ConsumableKind::Food, false, false).size() == 2);

    // The first effect is one no follower can use: nothing, rather than
    // the second effect standing in for it.
    const std::vector<Seen> firstUseless{{"Cure Disease", 0.0f, 0.0f, plain}, {"Fortify Health", 20.0f, 60.0f, plain}};
    REQUIRE(ConsumableEffectsOf(firstUseless, ConsumableKind::Ingredient, false, false).empty());
    REQUIRE(ConsumableEffectsOf(firstUseless, ConsumableKind::Potion, false, false).size() == 1);
}

TEST_CASE("a skill fortify counts as a buff only where a perk reads it", "[effects]")
{
    using Seen = ConsumableEffectSeen;
    EffectShape skill;
    skill.valueModifier = true;
    skill.peakValue = true;
    skill.skillModifier = true;
    skill.duration = 60.0f;
    const std::vector<Seen> one{{"Fortify One-handed", 20.0f, 60.0f, skill}};
    REQUIRE_FALSE(ConsumableEffectsOf(one, ConsumableKind::Potion, false, false)[0].buff);
    REQUIRE(ConsumableEffectsOf(one, ConsumableKind::Potion, true, false)[0].buff);
}

TEST_CASE("the effect picks are one list, one per name, by name", "[effects][picks]")
{
    constexpr std::uint32_t kOakflesh = 0x0005AD5C;
    constexpr std::uint32_t kFlameCloak = 0x0003AEA2;
    constexpr std::uint32_t kPotionRegen = 0x0003EB06;
    constexpr std::uint32_t kFoodRegen = 0xFE000800;
    constexpr std::uint32_t kBloodSacrifice = 0xFE00098D;

    const auto picks = ArrangeEffectPicks({
        {"Oakflesh", kOakflesh},
        {"Fortify Health Regeneration", kPotionRegen},
        // A food's effect of the same name, another record: one pick, the
        // first given.
        {"Fortify Health Regeneration", kFoodRegen},
        // A scroll of a spell known: the same effect again.
        {"Oakflesh", kOakflesh},
        {"Flame Cloak", kFlameCloak},
        {"Blood Sacrifice", kBloodSacrifice},
        // Nothing that lasts, or no name: nothing to pick.
        {"Firebolt", 0},
        {"", 0x1234},
    });
    REQUIRE(picks.size() == 4);
    REQUIRE(picks[0].name == "Blood Sacrifice");
    REQUIRE(picks[1].name == "Flame Cloak");
    REQUIRE(picks[2].name == "Fortify Health Regeneration");
    REQUIRE(picks[2].effect == kPotionRegen);
    REQUIRE(picks[3].name == "Oakflesh");
    REQUIRE(ArrangeEffectPicks({}).empty());
}
