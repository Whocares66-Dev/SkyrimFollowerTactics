// Which spells the snapshot calls known, used, castable and active, from
// what the game reads of the records and the effect list.

#include <catch2/catch_test_macros.hpp>

#include "core/Spells.h"

#include <vector>

using namespace ft;

namespace
{

SpellSeen Shout(std::uint32_t id, int highestWord, bool wrapper = false)
{
    SpellSeen s;
    s.id = id;
    s.kind = SpellSeen::Kind::Shout;
    s.highestWord = highestWord;
    s.wrapper = wrapper;
    return s;
}

SpellSeen Scroll(std::uint32_t id, int carried)
{
    SpellSeen s;
    s.id = id;
    s.kind = SpellSeen::Kind::Scroll;
    s.carried = carried;
    return s;
}

SpellSeen Power(std::uint32_t id, bool greater, bool usedToday)
{
    SpellSeen s;
    s.id = id;
    s.kind = SpellSeen::Kind::Power;
    s.greater = greater;
    s.usedToday = usedToday;
    return s;
}

SpellSeen Spell(std::uint32_t id, bool castable)
{
    SpellSeen s;
    s.id = id;
    s.castable = castable;
    return s;
}

} // namespace

TEST_CASE("known: a shout with a word, a scroll carried, a power, a castable spell; in the order seen", "[spells]")
{
    const std::vector<SpellSeen> seen{
        Shout(0x13E07, 0),    Shout(0x13E08, -1),         Shout(0x1000, 2, true),      Scroll(0x9659B, 2),
        Scroll(0x9659C, 0),   Power(0xE40C3, true, true), Power(0xE40C4, true, false), Power(0x3A7D5, false, true),
        Spell(0x12FCD, true), Spell(0x12FCE, false)};
    const SpellsKnown known = ClassifySpells(seen);
    REQUIRE(known.known == std::vector<std::uint32_t>{0x13E07, 0x9659B, 0xE40C3, 0xE40C4, 0x3A7D5, 0x12FCD});
    // Only a greater power on the used list is used today: a lesser power
    // the engine happens to list is not.
    REQUIRE(known.usedToday == std::vector<std::uint32_t>{0xE40C3});
    REQUIRE(known.castable == std::vector<std::uint32_t>{0x12FCD});
    REQUIRE(ClassifySpells({}).known.empty());
}

TEST_CASE("active: an effect with time left names its spell, and its shout when the spell is a word's", "[spells]")
{
    const std::vector<ShoutWords> shouts{{0x13E07, {0x13E0A, 0x13E0B, 0x13E0C}}, {0x13E08, {0x13E0D}}};
    std::vector<EffectSeen> effects;
    effects.push_back({0x12FCD, 60.0f, 10.0f}); // Oakflesh, running
    effects.push_back({0x13E0B, 8.0f, 2.0f});   // Become Ethereal's second word
    effects.push_back({0x3EADE, 0.0f, 0.0f});   // an instant heal: already happened
    effects.push_back({0x12FCE, 5.0f, 5.0f});   // expired
    effects.push_back({0, 5.0f, 1.0f});         // no spell
    effects.push_back({0x13E0D, 3.0f, 0.5f});   // the other shout's word
    const auto active = ActiveSpells(effects, shouts);
    REQUIRE(active == std::vector<std::uint32_t>{0x12FCD, 0x13E0B, 0x13E07, 0x13E0D, 0x13E08});
    REQUIRE(ActiveSpells({}, shouts).empty());
    REQUIRE(ActiveSpells(effects, {}) == std::vector<std::uint32_t>{0x12FCD, 0x13E0B, 0x13E0D});
}

TEST_CASE("the time left on a source's effect: the longest, none for an unrelated or instant one", "[spells]")
{
    std::vector<EffectSeen> effects;
    effects.push_back({0x13E0A, 10.0f, 4.0f});  // a word: 6 left
    effects.push_back({0x13E0B, 30.0f, 29.0f}); // another word: 1 left
    effects.push_back({0x12FCD, 60.0f, 0.0f});  // unrelated
    effects.push_back({0x13E0C, 0.0f, 0.0f});   // instant
    const std::vector<std::uint32_t> words{0x13E0A, 0x13E0B, 0x13E0C};
    REQUIRE(RemainingOn(effects, words) == 6.0f);
    REQUIRE(RemainingOn(effects, std::vector<std::uint32_t>{0x3EADE}) == 0.0f);
    REQUIRE(RemainingOn({}, words) == 0.0f);
}
