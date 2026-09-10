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

TEST_CASE("dual wielding is one-handed weapon against one-handed weapon, and nothing else")
{
    Holdable dagger = Thing(kSteelDagger, Grip::Either);
    dagger.kind = Kind::Weapon;
    Holdable sword = Thing(10, Grip::Either);
    sword.kind = Kind::Weapon;
    Holdable shield = Thing(kIronShield, Grip::LeftOnly);
    shield.kind = Kind::Weapon;
    Holdable bow = Thing(kHuntingBow, Grip::Both);
    bow.kind = Kind::Weapon;
    Holdable flames = Thing(kFlames, Grip::Either);
    flames.kind = Kind::Spell;

    // A second one-hander against the first: that is the thing.
    CHECK(WouldDualWield(dagger, &sword));
    CHECK(WouldDualWield(sword, &dagger));
    // An empty other hand, a shield, a spell, a bow there: no.
    CHECK_FALSE(WouldDualWield(dagger, nullptr));
    CHECK_FALSE(WouldDualWield(dagger, &shield));
    CHECK_FALSE(WouldDualWield(dagger, &flames));
    CHECK_FALSE(WouldDualWield(dagger, &bow));
    // A shield or a spell coming in beside a sword: no.
    CHECK_FALSE(WouldDualWield(shield, &sword));
    CHECK_FALSE(WouldDualWield(flames, &sword));
    // The only dagger, asked into the other hand, moves; two daggers are
    // one in each hand.
    CHECK_FALSE(WouldDualWield(dagger, &dagger));
    dagger.count = 2;
    CHECK(WouldDualWield(dagger, &dagger));
}

TEST_CASE("where the style forbids dual wielding, a pinned one-hander gives way to one in the other hand")
{
    Holdable dagger = Thing(kSteelDagger, Grip::Either);
    dagger.kind = Kind::Weapon;
    constexpr std::uint32_t kSword = 13; // an iron sword, Either
    Holdable sword = Thing(kSword, Grip::Either);
    sword.kind = Kind::Weapon;
    Holdable shield = Thing(kIronShield, Grip::LeftOnly);
    shield.kind = Kind::Weapon;

    // With dual wielding allowed the two hands are two places, as ever.
    CHECK_FALSE(Conflicts(dagger, Hand::Left, sword, Hand::Right));
    // Without it, a one-hander pinned in the other hand is in the way; a
    // shield there, or a shield coming in, is not.
    CHECK(Conflicts(dagger, Hand::Left, sword, Hand::Right, false));
    CHECK(Conflicts(dagger, Hand::Right, sword, Hand::Left, false));
    CHECK_FALSE(Conflicts(dagger, Hand::Left, shield, Hand::Right, false));
    CHECK_FALSE(Conflicts(shield, Hand::Left, sword, Hand::Right, false));
    // The same hand conflicts as before, either way.
    CHECK(Conflicts(dagger, Hand::Right, sword, Hand::Right, false));

    // Pinning the dagger left releases the sword pinned right, whole, and
    // says so -- the game side takes the sword off, since the engine's
    // equip of the left hand leaves the right hand alone.
    std::vector<Pin> pins;
    AddPin(pins, sword, Hand::Right, false);
    const auto displaced = MakeRoom(pins, dagger, Hand::Left, false);
    REQUIRE(displaced.size() == 1);
    CHECK(displaced[0].form == kSword);
    CHECK(displaced[0].hands == Hand::Right);
    CHECK(pins.empty());

    // With dual wielding allowed the sword stays pinned beside the dagger.
    AddPin(pins, sword, Hand::Right, false);
    CHECK(MakeRoom(pins, dagger, Hand::Left, true).empty());
    CHECK(pins.size() == 1);

    // A shield pinned left is no bar to a sword right, and an equipped
    // (unpinned) sword is no pin to release: the panel greys nothing for
    // it, and the AI swaps as it likes.
    pins.clear();
    AddPin(pins, shield, Hand::Left, false);
    CHECK(MakeRoom(pins, sword, Hand::Right, false).empty());
    CHECK(pins.size() == 1);
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

TEST_CASE("one copy of a pinned weapon has no second copy for the other hand")
{
    // A staff pinned right and the left hand free. The AI's staff-in-left
    // entry would put the one staff in both hands, so it goes; a second
    // staff in the bag could go left, and stays.
    Holdable staff = Thing(kSteelDagger, Grip::Either);
    const std::vector<Pin> pins{{staff, Hand::Right}};
    CHECK_FALSE(KeptFromAI(pins, staff, Hand::Right));
    CHECK(KeptFromAI(pins, staff, Hand::Left));
    staff.count = 2;
    CHECK_FALSE(KeptFromAI(pins, staff, Hand::Left));
    // A spell is never short of copies.
    Holdable flames = Thing(kFlames, Grip::Either);
    flames.count = 2;
    const std::vector<Pin> spellPin{{flames, Hand::Right}};
    CHECK_FALSE(KeptFromAI(spellPin, flames, Hand::Left));
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

// ---- The fight is over.

TEST_CASE("a fight that began with nothing pinned takes nothing off")
{
    // The rules pinned a bow and a cuirass for the fight. Both are let go
    // in place: the gear stays on, the AI's to change. Nothing comes back
    // because nothing was there. This is the case that stripped Jenassa.
    const std::vector<Pin> before;
    const std::vector<Pin> now{{Thing(kHuntingBow, Grip::Both), Hand::Both}, {Armour(kIronArmor, 0x4), Hand::None}};

    const AfterFight settle = SettleAfterFight(now, before);
    REQUIRE(settle.released.size() == 2);
    CHECK(settle.released[0].form == kHuntingBow);
    CHECK(settle.released[0].hands == Hand::Both);
    CHECK_FALSE(settle.released[0].takeOff);
    CHECK(settle.released[1].form == kIronArmor);
    CHECK_FALSE(settle.released[1].takeOff);
    CHECK(settle.restored.empty());
}

TEST_CASE("what was pinned before the fight comes back, and only that")
{
    // Before: a dagger in the right hand and robes. The fight pinned a bow
    // -- both hands, over the dagger -- and iron armour over the robes,
    // and a helmet in a slot nothing before it used. The bow and the
    // armour come off to make way; the helmet stays on, unpinned; the
    // dagger and the robes are pinned again.
    const std::vector<Pin> before{{Thing(kSteelDagger, Grip::Either), Hand::Right}, {Armour(10, 0x4), Hand::None}};
    const std::vector<Pin> now{{Thing(kHuntingBow, Grip::Both), Hand::Both},
                               {Armour(kIronArmor, 0x4), Hand::None},
                               {Armour(11, 0x2), Hand::None}};

    const AfterFight settle = SettleAfterFight(now, before);
    REQUIRE(settle.released.size() == 3);
    CHECK(settle.released[0].form == kHuntingBow);
    CHECK(settle.released[0].takeOff);
    CHECK(settle.released[1].form == kIronArmor);
    CHECK(settle.released[1].takeOff);
    CHECK(settle.released[2].form == 11);
    CHECK_FALSE(settle.released[2].takeOff);
    REQUIRE(settle.restored.size() == 2);
    CHECK(settle.restored[0].thing.form == kSteelDagger);
    CHECK(settle.restored[0].hands == Hand::Right);
    CHECK(settle.restored[1].thing.form == 10);
}

TEST_CASE("a pin kept through the fight, or made in the panel during it, is untouched")
{
    // The helmet was pinned before and is pinned still: not released, not
    // restored. The sword the rules pinned in a hand nothing before it
    // used is let go in place.
    const std::vector<Pin> before{{Armour(kIronArmor, 0x2), Hand::None}};
    const std::vector<Pin> now{{Armour(kIronArmor, 0x2), Hand::None}, {Thing(kSteelDagger, Grip::Either), Hand::Right}};

    const AfterFight settle = SettleAfterFight(now, before);
    REQUIRE(settle.released.size() == 1);
    CHECK(settle.released[0].form == kSteelDagger);
    CHECK_FALSE(settle.released[0].takeOff);
    CHECK(settle.restored.empty());

    // The same pin in other hands is a change: the fight moved the dagger
    // to the left, and it comes back to the right. Not taken off -- it is
    // the same thing -- the watchdog puts it in the right hand.
    const std::vector<Pin> moved{{Armour(kIronArmor, 0x2), Hand::None},
                                 {Thing(kSteelDagger, Grip::Either), Hand::Left}};
    const std::vector<Pin> wasRight{{Armour(kIronArmor, 0x2), Hand::None},
                                    {Thing(kSteelDagger, Grip::Either), Hand::Right}};
    const AfterFight back = SettleAfterFight(moved, wasRight);
    REQUIRE(back.released.size() == 1);
    CHECK_FALSE(back.released[0].takeOff);
    REQUIRE(back.restored.size() == 1);
    CHECK(back.restored[0].hands == Hand::Right);
}

// ---- The watchdog against the game's hands, simulated.
//
// There is no headless Skyrim, so what the engine does with a hand is
// written down here as it was learned in play, and the watchdog's rule is
// run against it. The three facts, each verified in game (CLAUDE.md, the
// SKSE gotchas):
//   1. Our own equip puts an item in a hand with the prevent-removal flag,
//      the pin, which holds against the engine's equip-best swap.
//   2. The engine's own equips land in our detour of EquipObject, which
//      refuses one that would break a pin -- the sword over pinned Flames
//      when a fight ends -- unless the thing is the pinned one itself.
//   3. EquipSpell is not detoured, and a spell going into a hand displaces
//      whatever is there, pinned or not: a pinned dagger comes off the
//      moment a spell wants the hand. The UseMagic package our cast rules
//      run does exactly this for the spell they cast.
namespace
{

struct Hands
{
    std::uint32_t left{0};
    std::uint32_t right{0};
};

struct World
{
    Hands hands;
    std::vector<Pin> pins;
    bool fighting{false};
    bool casting{false}; // one of OUR casts holds a package record
    int putBacks{0};

    // 1. Our equip: the pin's thing into the pin's hands.
    void OurEquip(const Holdable &thing, Hand into)
    {
        if (Overlap(into, Hand::Left))
            hands.left = thing.form;
        if (Overlap(into, Hand::Right))
            hands.right = thing.form;
    }

    // 2. The engine's equip, through the detour: refused against a pin.
    bool EngineEquip(const Holdable &thing, Hand into)
    {
        if (!FindPin(pins, thing.form))
        {
            for (const Pin &pin : pins)
                if (Conflicts(thing, into, pin.thing, pin.hands))
                    return false;
        }
        OurEquip(thing, into);
        return true;
    }

    // 3. A spell into a hand, by the package or the AI: displaces, always.
    void EquipSpell(const Holdable &spell, Hand hand)
    {
        OurEquip(spell, hand);
    }

    [[nodiscard]] bool On(const Pin &pin) const
    {
        const bool left = !Overlap(pin.hands, Hand::Left) || hands.left == pin.thing.form;
        const bool right = !Overlap(pin.hands, Hand::Right) || hands.right == pin.thing.form;
        return left && right;
    }

    // The watchdog's tick: each pin, back on when PutBackNow says so.
    void Watchdog()
    {
        for (const Pin &pin : pins)
        {
            if (PutBackNow(pin, On(pin), fighting, casting))
            {
                OurEquip(pin.thing, pin.hands);
                ++putBacks;
            }
        }
    }
};

constexpr std::uint32_t kIronDagger = 12; // Either
constexpr std::uint32_t kIronSword = 13;  // Either

} // namespace

TEST_CASE("when a pin goes back: at once, except a hand our cast is using")
{
    const Pin dagger{Thing(kSteelDagger, Grip::Either), Hand::Left};
    const Pin armour{Armour(kIronArmor, 0x4), Hand::None};

    // On already: nothing to do, whatever else is true.
    for (const bool fighting : {false, true})
        for (const bool casting : {false, true})
        {
            CHECK_FALSE(PutBackNow(dagger, true, fighting, casting));
            CHECK_FALSE(PutBackNow(armour, true, fighting, casting));
        }
    // Off, out of a fight: back, cast or no cast (a cast out of combat is
    // not ours to worry about; the package only runs in one).
    CHECK(PutBackNow(dagger, false, false, false));
    CHECK(PutBackNow(dagger, false, false, true));
    // Off, in a fight: back -- unless our cast has the hands.
    CHECK(PutBackNow(dagger, false, true, false));
    CHECK_FALSE(PutBackNow(dagger, false, true, true));
    // Armour and ammunition are contested by no cast.
    CHECK(PutBackNow(armour, false, true, true));
}

TEST_CASE("a dagger in each hand survives a Lightning Bolt cast, and comes back after it")
{
    // Marcurio, 2026-09-04 12:37: Iron Dagger pinned right, Steel Dagger
    // pinned left, a rule casting Lightning Bolt (a left-hand spell) when
    // the player is attacked. The cast put the bolt in the left hand and
    // the dagger came off; with the watchdog standing down for the whole
    // fight, it stayed off, still pinned, to the end of the fight.
    World w;
    const Holdable iron = Thing(kIronDagger, Grip::Either);
    const Holdable steel = Thing(kSteelDagger, Grip::Either);
    const Holdable bolt = Thing(kLightningBolt, Grip::LeftOnly);
    w.pins = {{iron, Hand::Right}, {steel, Hand::Left}};
    w.OurEquip(iron, Hand::Right);
    w.OurEquip(steel, Hand::Left);
    REQUIRE(w.hands.left == kSteelDagger);
    REQUIRE(w.hands.right == kIronDagger);

    // The fight begins; nothing has moved, and the watchdog does nothing.
    w.fighting = true;
    w.Watchdog();
    CHECK(w.putBacks == 0);

    // The cast rule fires: the package takes the left hand for the bolt,
    // and the dagger is off. While the cast is in progress the watchdog
    // leaves the hand alone -- putting the dagger back now would knock the
    // bolt out mid-cast.
    w.casting = true;
    w.EquipSpell(bolt, Hand::Left);
    REQUIRE(w.hands.left == kLightningBolt);
    w.Watchdog();
    CHECK(w.hands.left == kLightningBolt);
    CHECK(w.putBacks == 0);

    // The bolt has left the hand; the record is released. The next tick
    // puts the dagger back, and the pins are as they were.
    w.casting = false;
    w.Watchdog();
    CHECK(w.hands.left == kSteelDagger);
    CHECK(w.hands.right == kIronDagger);
    CHECK(w.putBacks == 1);
    REQUIRE(w.pins.size() == 2);
    CHECK(w.pins[0].thing.form == kIronDagger);
    CHECK(w.pins[1].thing.form == kSteelDagger);

    // The second cast, two seconds on: the same borrow, the same return.
    w.casting = true;
    w.EquipSpell(bolt, Hand::Left);
    w.Watchdog();
    CHECK(w.hands.left == kLightningBolt);
    w.casting = false;
    w.Watchdog();
    CHECK(w.hands.left == kSteelDagger);
    CHECK(w.putBacks == 2);

    // Meanwhile the engine's own choices are refused against the pins: the
    // sword it would put in the right hand, the bow that wants both.
    CHECK_FALSE(w.EngineEquip(Thing(kIronSword, Grip::Either), Hand::Right));
    CHECK_FALSE(w.EngineEquip(Thing(kHuntingBow, Grip::Both), Hand::Both));
    CHECK(w.hands.right == kIronDagger);
    // The pinned thing itself always passes, whichever hand.
    CHECK(w.EngineEquip(iron, Hand::Right));

    // Out of the fight, the same rule holds: a dagger knocked out by
    // anything comes straight back.
    w.fighting = false;
    w.EquipSpell(bolt, Hand::Left);
    w.Watchdog();
    CHECK(w.hands.left == kSteelDagger);
}

TEST_CASE("a voice pin: one power or shout readied, the rest set aside", "[pins]")
{
    // A power and a shout are Voice things: no hand, one slot between them,
    // like two quivers. Pinning one displaces the other and keeps every
    // other voice entry from the AI; a spell in a hand is untouched.
    Holdable battleCry;
    battleCry.form = 0x000E40C3;
    battleCry.kind = Kind::Voice;
    Holdable unrelentingForce;
    unrelentingForce.form = 0x00013E07;
    unrelentingForce.kind = Kind::Voice;
    Holdable firebolt;
    firebolt.form = 0x00012FCD;
    firebolt.kind = Kind::Spell;
    firebolt.grip = Grip::Either;

    std::vector<Pin> pins;
    REQUIRE(Pinnable(battleCry));
    REQUIRE(HandsFor(battleCry.grip, Hand::Right) == Hand::None);
    AddPin(pins, battleCry, Hand::None, false);
    REQUIRE(pins.size() == 1);

    REQUIRE(Conflicts(unrelentingForce, Hand::None, battleCry, Hand::None));
    REQUIRE_FALSE(Conflicts(firebolt, Hand::Right, battleCry, Hand::None));
    REQUIRE(SetAside(pins, unrelentingForce));
    REQUIRE_FALSE(SetAside(pins, battleCry));
    REQUIRE_FALSE(SetAside(pins, firebolt));
    REQUIRE(KeptFromAI(pins, unrelentingForce, Hand::None));
    REQUIRE_FALSE(KeptFromAI(pins, battleCry, Hand::None));
    REQUIRE_FALSE(KeptFromAI(pins, firebolt, Hand::Left));

    const auto why = Shadowing(pins, unrelentingForce);
    REQUIRE(why.size() == 1);
    REQUIRE(why[0].thing.form == battleCry.form);

    // Pinning the shout makes room: the power's pin goes, whole.
    const auto displaced = MakeRoom(pins, unrelentingForce, Hand::None);
    REQUIRE(displaced.size() == 1);
    REQUIRE(displaced[0].form == battleCry.form);
    AddPin(pins, unrelentingForce, Hand::None, false);
    REQUIRE(pins.size() == 1);
    REQUIRE(pins[0].thing.form == unrelentingForce.form);
}

TEST_CASE("a ban is a set of forms, once each", "[loadout]")
{
    Bans bans;
    REQUIRE_FALSE(IsBanned(bans, 0x13989));
    REQUIRE(Ban(bans, 0x13989));
    REQUIRE_FALSE(Ban(bans, 0x13989)); // already
    REQUIRE_FALSE(Ban(bans, 0));       // nothing is not a thing
    REQUIRE(IsBanned(bans, 0x13989));
    REQUIRE(bans.size() == 1);
    REQUIRE(Unban(bans, 0x13989));
    REQUIRE_FALSE(Unban(bans, 0x13989));
    REQUIRE(bans.empty());
}
