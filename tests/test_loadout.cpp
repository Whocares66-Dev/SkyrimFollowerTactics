// The pin rules, as learned in play on 2026-09-03 and written down here so
// they cannot drift. No Skyrim, no SKSE: the planner is pure.

#include <catch2/catch_test_macros.hpp>

#include "core/Loadout.h"

using namespace ft;

namespace
{

Holdable Thing(std::uint32_t form, Grip grip)
{
    Holdable t;
    t.form = form;
    t.grip = grip;
    return t;
}

Holdable Armour(std::uint32_t form, std::uint32_t slots)
{
    Holdable t;
    t.form = form;
    t.slots = slots;
    return t;
}

// Marcurio's spells, as the engine labelled them: his Firebolt and Chain
// Lightning are the NPC right-hand variants, his Lightning Bolt and Close
// Wounds the left-hand ones, Flames and the rest take either hand.
constexpr std::uint32_t kFirebolt = 1;       // RightOnly
constexpr std::uint32_t kLightningBolt = 2;  // LeftOnly
constexpr std::uint32_t kFlames = 3;         // Either
constexpr std::uint32_t kStoneflesh = 4;     // Either, self
constexpr std::uint32_t kChainLightning = 5; // RightOnly, Adept: above his skill
constexpr std::uint32_t kSteelDagger = 6;    // Either
constexpr std::uint32_t kHuntingBow = 7;     // Both
constexpr std::uint32_t kIronShield = 8;     // LeftOnly
constexpr std::uint32_t kIronArmor = 9;      // no hand

} // namespace

TEST_CASE("the hands a thing takes when pinned")
{
    // A one-particular-hand thing takes that hand whatever was asked: the
    // NPC spell variants, a shield, a torch.
    CHECK(HandsFor(Grip::LeftOnly, Hand::Right) == Hand::Left);
    CHECK(HandsFor(Grip::RightOnly, Hand::Left) == Hand::Right);
    // A two-hander takes both.
    CHECK(HandsFor(Grip::Both, Hand::Left) == Hand::Both);
    // An either-hand thing goes where asked, and to the right when nothing
    // was asked -- the engine's own default for an either-hand spell.
    CHECK(HandsFor(Grip::Either, Hand::Left) == Hand::Left);
    CHECK(HandsFor(Grip::Either, Hand::Right) == Hand::Right);
    CHECK(HandsFor(Grip::Either, Hand::None) == Hand::Right);
    // Armour takes no hand however it is asked.
    CHECK(HandsFor(Grip::None, Hand::Left) == Hand::None);
}

TEST_CASE("a spell above her skill can be equipped but not pinned")
{
    Holdable chain = Thing(kChainLightning, Grip::RightOnly);
    CHECK(Pinnable(chain));
    chain.unusable = true;
    CHECK_FALSE(Pinnable(chain));
}

TEST_CASE("what competes with a right-hand pin")
{
    // The coarse rule, for an entry whose hand is not known. The AI put
    // Flames straight into the pinned right hand: either-hand competes with
    // any pin. A left-only spell has its own hand and does not.
    CHECK(Competes(Grip::RightOnly, Hand::Right));
    CHECK(Competes(Grip::Either, Hand::Right));
    CHECK(Competes(Grip::Both, Hand::Right));
    CHECK_FALSE(Competes(Grip::LeftOnly, Hand::Right));
    CHECK_FALSE(Competes(Grip::None, Hand::Right));
}

TEST_CASE("what competes with a left-hand pin")
{
    CHECK(Competes(Grip::LeftOnly, Hand::Left));
    CHECK(Competes(Grip::Either, Hand::Left));
    CHECK(Competes(Grip::Both, Hand::Left));
    CHECK_FALSE(Competes(Grip::RightOnly, Hand::Left));
}

TEST_CASE("with nothing pinned nothing competes")
{
    for (const Grip grip : {Grip::None, Grip::LeftOnly, Grip::RightOnly, Grip::Either, Grip::Both})
        CHECK_FALSE(Competes(grip, Hand::None));
}

TEST_CASE("the AI's list holds an either-hand spell once per hand, and a pin keeps one")
{
    // Flames pinned left, Firebolt pinned right: the AI's Flames-in-right
    // would undo the Firebolt pin, so it goes; Flames-in-left is the pin.
    const std::vector<Pin> pins{{kFlames, Hand::Left}, {kFirebolt, Hand::Right}};
    const Holdable flames = Thing(kFlames, Grip::Either);
    CHECK_FALSE(KeptFromAI(pins, flames, Hand::Left));
    CHECK(KeptFromAI(pins, flames, Hand::Right));
    CHECK_FALSE(KeptFromAI(pins, Thing(kFirebolt, Grip::RightOnly), Hand::Right));
    // Everything else with a hand goes, the self-cast Stoneflesh included:
    // both hands pinned means both hands are spoken for.
    CHECK(KeptFromAI(pins, Thing(kStoneflesh, Grip::Either), Hand::Left));
    CHECK(KeptFromAI(pins, Thing(kStoneflesh, Grip::Either), Hand::Right));
    CHECK(KeptFromAI(pins, Thing(kSteelDagger, Grip::Either), Hand::Right));
    CHECK(KeptFromAI(pins, Thing(kHuntingBow, Grip::Both), Hand::Both));
    CHECK(KeptFromAI(pins, Thing(kIronShield, Grip::LeftOnly), Hand::Left));
    // Armour has no hand and is left to the AI.
    CHECK_FALSE(KeptFromAI(pins, Armour(kIronArmor, 0x4), Hand::None));
}

TEST_CASE("with only the right hand pinned, the left stays the AI's")
{
    const std::vector<Pin> pins{{kFirebolt, Hand::Right}};
    const Holdable flames = Thing(kFlames, Grip::Either);
    CHECK_FALSE(KeptFromAI(pins, flames, Hand::Left));
    CHECK(KeptFromAI(pins, flames, Hand::Right));
    CHECK_FALSE(KeptFromAI(pins, Thing(kLightningBolt, Grip::LeftOnly), Hand::Left));
    CHECK_FALSE(KeptFromAI(pins, Thing(kIronShield, Grip::LeftOnly), Hand::Left));
    // A two-hander wants the pinned hand too.
    CHECK(KeptFromAI(pins, Thing(kHuntingBow, Grip::Both), Hand::Both));
    CHECK(KeptFromAI(pins, Thing(kSteelDagger, Grip::Either), Hand::Right));
}

TEST_CASE("an entry that carries no hand falls back to the coarse rule")
{
    const std::vector<Pin> pins{{kFirebolt, Hand::Right}};
    // Either-hand, hand unknown: it could land in the pinned hand, so it goes.
    CHECK(KeptFromAI(pins, Thing(kFlames, Grip::Either), Hand::None));
    CHECK_FALSE(KeptFromAI(pins, Thing(kLightningBolt, Grip::LeftOnly), Hand::None));
    // The pin itself, hand unknown, is left alone.
    CHECK_FALSE(KeptFromAI(pins, Thing(kFirebolt, Grip::RightOnly), Hand::None));
    // Nothing pinned, nothing kept.
    CHECK_FALSE(KeptFromAI({}, Thing(kFlames, Grip::Either), Hand::Right));
}

TEST_CASE("what the panel greys out: things with no hand left to take")
{
    const std::vector<Pin> right{{kFirebolt, Hand::Right}};
    CHECK_FALSE(SetAside(right, Thing(kFlames, Grip::Either)));
    CHECK_FALSE(SetAside(right, Thing(kLightningBolt, Grip::LeftOnly)));
    CHECK(SetAside(right, Thing(kChainLightning, Grip::RightOnly)));
    CHECK(SetAside(right, Thing(kHuntingBow, Grip::Both)));
    CHECK_FALSE(SetAside(right, Thing(kFirebolt, Grip::RightOnly)));
    CHECK_FALSE(SetAside(right, Armour(kIronArmor, 0x4)));

    const std::vector<Pin> both{{kFlames, Hand::Left}, {kFirebolt, Hand::Right}};
    CHECK(SetAside(both, Thing(kStoneflesh, Grip::Either)));
    CHECK(SetAside(both, Thing(kSteelDagger, Grip::Either)));
    CHECK_FALSE(SetAside(both, Thing(kFlames, Grip::Either)));
    CHECK_FALSE(SetAside(both, Armour(kIronArmor, 0x4)));
}

TEST_CASE("pinned hands are the union of the pins")
{
    CHECK(PinnedHands({}) == Hand::None);
    CHECK(PinnedHands({{kFirebolt, Hand::Right}}) == Hand::Right);
    CHECK(PinnedHands({{kFirebolt, Hand::Right}, {kLightningBolt, Hand::Left}}) == Hand::Both);
    CHECK(PinnedHands({{kIronArmor, Hand::None}}) == Hand::None);
}

TEST_CASE("pinning releases what it displaces")
{
    const Holdable dagger = Thing(kSteelDagger, Grip::Either);
    const Holdable shield = Thing(kIronShield, Grip::LeftOnly);
    const Holdable bow = Thing(kHuntingBow, Grip::Both);
    const Holdable cuirass = Armour(kIronArmor, 0x4);
    const Holdable robes = Armour(10, 0x4);
    const Holdable boots = Armour(11, 0x80);

    // Hands that overlap.
    CHECK(Conflicts(dagger, Hand::Left, shield, Hand::Left));
    CHECK_FALSE(Conflicts(dagger, Hand::Right, shield, Hand::Left));
    CHECK(Conflicts(bow, Hand::Both, shield, Hand::Left));
    // Armour on shared body slots -- pinned iron armour blocked robes until
    // the robes' pin released it.
    CHECK(Conflicts(robes, Hand::None, cuirass, Hand::None));
    CHECK_FALSE(Conflicts(boots, Hand::None, cuirass, Hand::None));
    // A hand thing and armour live alongside.
    CHECK_FALSE(Conflicts(dagger, Hand::Right, cuirass, Hand::None));
    // Ammunition against ammunition, and only that.
    Holdable arrows = Thing(12, Grip::None);
    arrows.ammo = true;
    Holdable bolts = Thing(13, Grip::None);
    bolts.ammo = true;
    CHECK(Conflicts(arrows, Hand::None, bolts, Hand::None));
    CHECK_FALSE(Conflicts(arrows, Hand::None, cuirass, Hand::None));
}
