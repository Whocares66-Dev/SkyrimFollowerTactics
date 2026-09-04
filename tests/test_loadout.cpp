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
    const std::vector<Pin> pins{{Thing(kFlames, Grip::Either), Hand::Left},
                                {Thing(kFirebolt, Grip::RightOnly), Hand::Right}};
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
    const std::vector<Pin> pins{{Thing(kFirebolt, Grip::RightOnly), Hand::Right}};
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
    const std::vector<Pin> pins{{Thing(kFirebolt, Grip::RightOnly), Hand::Right}};
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
    const std::vector<Pin> right{{Thing(kFirebolt, Grip::RightOnly), Hand::Right}};
    CHECK_FALSE(SetAside(right, Thing(kFlames, Grip::Either)));
    CHECK_FALSE(SetAside(right, Thing(kLightningBolt, Grip::LeftOnly)));
    CHECK(SetAside(right, Thing(kChainLightning, Grip::RightOnly)));
    CHECK(SetAside(right, Thing(kHuntingBow, Grip::Both)));
    CHECK_FALSE(SetAside(right, Thing(kFirebolt, Grip::RightOnly)));
    CHECK_FALSE(SetAside(right, Armour(kIronArmor, 0x4)));

    const std::vector<Pin> both{{Thing(kFlames, Grip::Either), Hand::Left},
                                {Thing(kFirebolt, Grip::RightOnly), Hand::Right}};
    CHECK(SetAside(both, Thing(kStoneflesh, Grip::Either)));
    CHECK(SetAside(both, Thing(kSteelDagger, Grip::Either)));
    CHECK_FALSE(SetAside(both, Thing(kFlames, Grip::Either)));
    CHECK_FALSE(SetAside(both, Armour(kIronArmor, 0x4)));
}

TEST_CASE("a pin may hold both hands, and let one go")
{
    // Flames pinned left, then right: one pin, both hands. Letting the
    // left go leaves the right.
    CHECK((Hand::Left | Hand::Right) == Hand::Both);
    CHECK(Without(Hand::Both, Hand::Left) == Hand::Right);
    CHECK(Without(Hand::Both, Hand::Right) == Hand::Left);
    CHECK(Without(Hand::Right, Hand::Right) == Hand::None);
    CHECK(Without(Hand::Right, Hand::Left) == Hand::Right);
}

TEST_CASE("why a thing is set aside: the pins holding a hand it could take")
{
    const std::vector<Pin> both{{Thing(kFlames, Grip::Either), Hand::Left},
                                {Thing(kFirebolt, Grip::RightOnly), Hand::Right}};
    // Stoneflesh could go either way, and both ways are pinned.
    auto why = Shadowing(both, Thing(kStoneflesh, Grip::Either));
    REQUIRE(why.size() == 2);
    CHECK(why[0].thing.form == kFlames);
    CHECK(why[0].hands == Hand::Left);
    CHECK(why[1].thing.form == kFirebolt);
    CHECK(why[1].hands == Hand::Right);
    // A left-only spell names only the left pin.
    why = Shadowing(both, Thing(kLightningBolt, Grip::LeftOnly));
    REQUIRE(why.size() == 1);
    CHECK(why[0].thing.form == kFlames);
    // The pin itself is not set aside, so nothing shadows it.
    CHECK(Shadowing(both, Thing(kFlames, Grip::Either)).empty());
    // With one hand free an either-hand spell is not set aside: no reason.
    const std::vector<Pin> right{{Thing(kFirebolt, Grip::RightOnly), Hand::Right}};
    CHECK(Shadowing(right, Thing(kFlames, Grip::Either)).empty());
    // A two-hander pinned takes both hands from a one-hander; the reason
    // names only the hands the one-hander could have had.
    const std::vector<Pin> bow{{Thing(kHuntingBow, Grip::Both), Hand::Both}};
    why = Shadowing(bow, Thing(kIronShield, Grip::LeftOnly));
    REQUIRE(why.size() == 1);
    CHECK(why[0].hands == Hand::Left);
}

TEST_CASE("a pinned cuirass keeps other cuirasses off, and nothing else")
{
    // The pin carries the slots it covers; the engine will not swap over a
    // locked piece, so what shares a slot is set aside, as a spell is.
    const std::vector<Pin> pins{{Armour(kIronArmor, 0x4), Hand::None}};
    CHECK(SetAside(pins, Armour(10, 0x4)));        // robes
    CHECK_FALSE(SetAside(pins, Armour(11, 0x80))); // boots
    CHECK_FALSE(SetAside(pins, Armour(12, 0x40))); // a ring
    CHECK_FALSE(SetAside(pins, Armour(kIronArmor, 0x4)));
    // Body armour holds no hand: a spell is not set aside by it.
    CHECK_FALSE(SetAside(pins, Thing(kFlames, Grip::Either)));
    const auto why = Shadowing(pins, Armour(10, 0x4));
    REQUIRE(why.size() == 1);
    CHECK(why[0].thing.form == kIronArmor);
    CHECK(why[0].hands == Hand::None);
}

TEST_CASE("pinned ammunition keeps other ammunition off")
{
    Holdable arrows = Thing(12, Grip::None);
    arrows.kind = Kind::Ammo;
    Holdable bolts = Thing(13, Grip::None);
    bolts.kind = Kind::Ammo;
    const std::vector<Pin> pins{{arrows, Hand::None}};
    CHECK(SetAside(pins, bolts));
    CHECK_FALSE(SetAside(pins, Armour(kIronArmor, 0x4)));
    REQUIRE(Shadowing(pins, bolts).size() == 1);
}

TEST_CASE("pinned hands are the union of the pins")
{
    CHECK(PinnedHands({}) == Hand::None);
    CHECK(PinnedHands({{Thing(kFirebolt, Grip::RightOnly), Hand::Right}}) == Hand::Right);
    CHECK(PinnedHands({{Thing(kFirebolt, Grip::RightOnly), Hand::Right},
                       {Thing(kLightningBolt, Grip::LeftOnly), Hand::Left}}) == Hand::Both);
    CHECK(PinnedHands({{Armour(kIronArmor, 0x4), Hand::None}}) == Hand::None);
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
    arrows.kind = Kind::Ammo;
    Holdable bolts = Thing(13, Grip::None);
    bolts.kind = Kind::Ammo;
    CHECK(Conflicts(arrows, Hand::None, bolts, Hand::None));
    CHECK_FALSE(Conflicts(arrows, Hand::None, cuirass, Hand::None));
}

// ---- What a request does to the pins.

namespace
{
const Holdable kFlamesSpell = Thing(kFlames, Grip::Either);
const Holdable kDagger = Thing(kSteelDagger, Grip::Either);
const Holdable kBow = Thing(kHuntingBow, Grip::Both);
const Holdable kShield = Thing(kIronShield, Grip::LeftOnly);
const Holdable kCuirass = Armour(kIronArmor, 0x4);
} // namespace

TEST_CASE("pinning the other hand of an either-hand thing pins both")
{
    std::vector<Pin> pins;
    AddPin(pins, kFlamesSpell, HandsFor(kFlamesSpell.grip, Hand::Left), false);
    AddPin(pins, kFlamesSpell, HandsFor(kFlamesSpell.grip, Hand::Right), false);
    REQUIRE(pins.size() == 1);
    CHECK(pins[0].hands == Hand::Both);
    // Her one dagger moving from the left to the right leaves the left.
    std::vector<Pin> one;
    AddPin(one, kDagger, Hand::Left, false);
    AddPin(one, kDagger, Hand::Right, true);
    REQUIRE(one.size() == 1);
    CHECK(one[0].hands == Hand::Right);
}

TEST_CASE("a hand cell lets go of its own hand and no other")
{
    // Flames pinned in both: the left cell releases the left and the
    // right stays pinned; a second left click, with the pin now on the
    // right alone, touches nothing and acts on the left (16:04, when it
    // took the right off instead).
    std::vector<Pin> pins{{kFlamesSpell, Hand::Both}};
    CHECK(LetGo(pins, kFlamesSpell, HandsFor(kFlamesSpell.grip, Hand::Left)) == Hand::Left);
    REQUIRE(pins.size() == 1);
    CHECK(pins[0].hands == Hand::Right);
    CHECK(LetGo(pins, kFlamesSpell, HandsFor(kFlamesSpell.grip, Hand::Left)) == Hand::Left);
    REQUIRE(pins.size() == 1);
    CHECK(pins[0].hands == Hand::Right);
    CHECK(LetGo(pins, kFlamesSpell, HandsFor(kFlamesSpell.grip, Hand::Right)) == Hand::Right);
    CHECK(pins.empty());
}

TEST_CASE("a two-hander and a one-particular-hand thing let go whole")
{
    // A bow takes both hands whichever cell is clicked, so either cell
    // releases the whole pin; a shield is only ever in the left.
    std::vector<Pin> pins{{kBow, Hand::Both}, {kShield, Hand::Left}};
    CHECK(LetGo(pins, kBow, HandsFor(kBow.grip, Hand::Left)) == Hand::Both);
    REQUIRE(pins.size() == 1);
    CHECK(pins[0].thing.form == kIronShield);
    CHECK(LetGo(pins, kShield, HandsFor(kShield.grip, Hand::Right)) == Hand::Left);
    CHECK(pins.empty());
}

TEST_CASE("a thing with no hand lets go whole, and an unpinned thing is untouched")
{
    std::vector<Pin> pins{{kCuirass, Hand::None}, {kDagger, Hand::Right}};
    CHECK(LetGo(pins, kCuirass, Hand::None) == Hand::None);
    REQUIRE(pins.size() == 1);
    // Taking off an unpinned thing acts on the hand asked and leaves the
    // pins as they are.
    CHECK(LetGo(pins, kFlamesSpell, Hand::Left) == Hand::Left);
    REQUIRE(pins.size() == 1);
    CHECK(pins[0].thing.form == kSteelDagger);
}

TEST_CASE("making room releases only the hand a new pin takes")
{
    // Two daggers pinned, one per hand: a sword into the right takes the
    // right dagger's hand and leaves the left pinned.
    std::vector<Pin> pins{{kDagger, Hand::Both}};
    auto out = MakeRoom(pins, Thing(20, Grip::Either), Hand::Right);
    REQUIRE(out.size() == 1);
    CHECK(out[0].form == kSteelDagger);
    CHECK(out[0].hands == Hand::Right);
    REQUIRE(pins.size() == 1);
    CHECK(pins[0].hands == Hand::Left);
    // A bow takes both hands: the remaining dagger goes whole.
    out = MakeRoom(pins, kBow, Hand::Both);
    REQUIRE(out.size() == 1);
    CHECK(out[0].hands == Hand::Left);
    CHECK(pins.empty());
}

TEST_CASE("making room for armour goes by body slot")
{
    std::vector<Pin> pins{{kCuirass, Hand::None}, {kShield, Hand::Left}};
    // A ring shares no slot: nothing gives way.
    CHECK(MakeRoom(pins, Armour(12, 0x40), Hand::None).empty());
    REQUIRE(pins.size() == 2);
    // Robes over the cuirass: the cuirass pin goes whole, the shield stays.
    const auto out = MakeRoom(pins, Armour(10, 0x4), Hand::None);
    REQUIRE(out.size() == 1);
    CHECK(out[0].form == kIronArmor);
    CHECK(out[0].hands == Hand::None);
    REQUIRE(pins.size() == 1);
    CHECK(pins[0].thing.form == kIronShield);
}
