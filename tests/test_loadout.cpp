// The pin rules, as learned in play on 2026-09-03 and written down here so
// they cannot drift. No Skyrim, no SKSE: the planner is pure.

#include <catch2/catch_test_macros.hpp>

#include "core/Loadout.h"

#include <algorithm>

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

bool Has(const std::vector<std::uint32_t> &forms, std::uint32_t form)
{
    return std::find(forms.begin(), forms.end(), form) != forms.end();
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
    // The AI put Flames straight into the pinned right hand: either-hand
    // competes with any pin. A left-only spell has its own hand and does not.
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

TEST_CASE("with both hands pinned the AI is left with the pins and nothing else")
{
    const std::vector<Pin> pins{{kLightningBolt, Hand::Left}, {kFirebolt, Hand::Right}};
    const std::vector<Holdable> things{
        Thing(kFirebolt, Grip::RightOnly),  Thing(kLightningBolt, Grip::LeftOnly),
        Thing(kFlames, Grip::Either),       Thing(kStoneflesh, Grip::Either),
        Thing(kSteelDagger, Grip::Either),  Thing(kHuntingBow, Grip::Both),
        Thing(kIronShield, Grip::LeftOnly), Armour(kIronArmor, 0x4),
    };
    const auto keep = KeepFromAI(pins, things);

    // The pins themselves are never kept from the AI.
    CHECK_FALSE(Has(keep, kFirebolt));
    CHECK_FALSE(Has(keep, kLightningBolt));
    // Everything with a hand is -- the self-cast Stoneflesh included, by
    // design: both hands pinned means both hands are spoken for.
    CHECK(Has(keep, kFlames));
    CHECK(Has(keep, kStoneflesh));
    CHECK(Has(keep, kSteelDagger));
    CHECK(Has(keep, kHuntingBow));
    CHECK(Has(keep, kIronShield));
    // Armour has no hand and is left to the AI.
    CHECK_FALSE(Has(keep, kIronArmor));
}

TEST_CASE("with only the left hand pinned, right-only spells stay available")
{
    const std::vector<Pin> pins{{kLightningBolt, Hand::Left}};
    const std::vector<Holdable> things{Thing(kFirebolt, Grip::RightOnly), Thing(kFlames, Grip::Either),
                                       Thing(kIronShield, Grip::LeftOnly)};
    const auto keep = KeepFromAI(pins, things);
    CHECK_FALSE(Has(keep, kFirebolt));
    CHECK(Has(keep, kFlames));
    CHECK(Has(keep, kIronShield));
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
