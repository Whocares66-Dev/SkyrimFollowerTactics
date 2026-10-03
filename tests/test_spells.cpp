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

SpellSeen Staff(std::uint32_t id, int carried, bool charged)
{
    SpellSeen s;
    s.id = id;
    s.kind = SpellSeen::Kind::Staff;
    s.carried = carried;
    s.charged = charged;
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

TEST_CASE("a staff is known while carried, and one that cannot pay for a cast is spent as well", "[spells]")
{
    const std::vector<SpellSeen> seen{Staff(0x29B73, 1, true), Staff(0x29B74, 2, false), Staff(0x29B75, 0, true),
                                      Staff(0x29B76, 0, false)};
    const SpellsKnown known = ClassifySpells(seen);
    REQUIRE(known.known == std::vector<std::uint32_t>{0x29B73, 0x29B74});
    // Not one no longer carried: its rule names nothing, and says that.
    REQUIRE(known.spent == std::vector<std::uint32_t>{0x29B74});
    REQUIRE(known.castable.empty());
}

TEST_CASE("a cast takes the staff in hand that can pay, else the fullest in the bag", "[spells]")
{
    using Copies = std::vector<StaffCopy>;
    // In the bag: the fullest, the first of equals.
    REQUIRE(StaffCopyToCast(Copies{{false, 100.0f}, {false, 400.0f}, {false, 400.0f}}, 50.0f) == 1);
    // One in hand needs no equip, however much fuller the bag's.
    REQUIRE(StaffCopyToCast(Copies{{false, 400.0f}, {true, 60.0f}}, 50.0f) == 1);
    // Unless it cannot pay: then the bag's.
    REQUIRE(StaffCopyToCast(Copies{{true, 40.0f}, {false, 400.0f}}, 50.0f) == 1);
    // Exactly the cost pays.
    REQUIRE(StaffCopyToCast(Copies{{false, 50.0f}}, 50.0f) == 0);
    // None can: spent.
    REQUIRE_FALSE(StaffCopyToCast(Copies{{true, 40.0f}, {false, 49.0f}}, 50.0f));
    REQUIRE_FALSE(StaffCopyToCast(Copies{}, 50.0f));
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
