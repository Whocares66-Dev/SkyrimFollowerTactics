// The pin rules, as learned in play on 2026-09-03 and written down here so
// they cannot drift. No Skyrim, no SKSE: the planner is pure.

#include <catch2/catch_test_macros.hpp>

#include "core/Loadout.h"

// Frost Damage 20 points: an enchantment made at the table, as its recipe.
const std::vector<ft::EnchantEffect> kFrost{{0x3A9AD, 20.0f, 1, 0}};

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

TEST_CASE("a spell above their skill can be equipped but not pinned")
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
    // Their one dagger moving from the left to the right leaves the left.
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
//   1. Our own equip puts the pin's thing in the pin's hands, and passes
//      the detours below: only the engine's are refused.
//   2. The engine's equips of an item, a spell and a shout -- the AI's, a
//      script's, a package's -- land in our detours of EquipObject,
//      EquipSpell and EquipShout, which refuse a banned thing and one that
//      would break a pin -- the sword over pinned Flames when a fight ends
//      -- unless the thing is the pinned one itself, into its own hand.
//   3. The spell a record of OURS casts passes the spell detour, since
//      the package's equip of it is ours: it goes into the hand it needs
//      and displaces whatever is there, pinned or not. A pinned dagger
//      comes off the moment our cast wants the hand, and the watchdog
//      puts it back once the spell has left.
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
    Bans bans;
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

    // 2. The engine's equip of anything -- an item, a spell, a shout --
    //    through its detour: refused against a ban or a pin, by the rule
    //    the detours ask.
    bool EngineEquip(const Holdable &thing, Hand into)
    {
        if (RefusesEngineEquip(pins, bans, thing, into, true))
            return false;
        OurEquip(thing, into);
        return true;
    }

    // 3. The package's equip of the spell a record of ours casts: passes
    //    the detour, and displaces.
    void OurCast(const Holdable &spell, Hand hand)
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
    w.OurCast(bolt, Hand::Left);
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
    w.OurCast(bolt, Hand::Left);
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
    // The pinned thing itself passes into its own hand; into the other,
    // with one copy, the engine would show it in both, so that is refused.
    CHECK(w.EngineEquip(iron, Hand::Right));
    CHECK_FALSE(w.EngineEquip(iron, Hand::Left));

    // Out of the fight, the same rule holds: a dagger knocked out by
    // anything comes straight back.
    w.fighting = false;
    w.OurCast(bolt, Hand::Left);
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
    const Holdable sword = Thing(0x13989, Grip::Either);
    REQUIRE_FALSE(IsBanned(bans, sword));
    REQUIRE(Ban(bans, 0x13989));
    REQUIRE_FALSE(Ban(bans, 0x13989)); // already
    REQUIRE_FALSE(Ban(bans, 0));       // nothing is not a thing
    REQUIRE(IsBanned(bans, sword));
    REQUIRE(bans.size() == 1);
    REQUIRE(Unban(bans, 0x13989));
    REQUIRE_FALSE(Unban(bans, 0x13989));
    REQUIRE(bans.empty());
}

TEST_CASE("a ban on one variant of a form leaves the other variants alone", "[loadout]")
{
    // Two Nordic Carved Armors: the plain one the outfit gave, the
    // enchanted one the player did. Two names under one form.
    Bans bans;
    Holdable plain = Armour(kIronArmor, 0x4);
    plain.variant = ItemVariant{};
    Holdable enchanted = plain;
    enchanted.variant->enchantment = kFrost;
    Holdable whichever = plain;
    whichever.variant = std::nullopt;

    REQUIRE(Ban(bans, kIronArmor, enchanted.variant));
    CHECK(IsBanned(bans, enchanted));
    CHECK_FALSE(IsBanned(bans, plain));
    // A thing whose name is not known -- the combat AI's list is by form
    // -- is banned by any ban on the form.
    CHECK(IsBanned(bans, whichever));
    // The same name once; the plain one beside it.
    REQUIRE_FALSE(Ban(bans, kIronArmor, enchanted.variant));
    REQUIRE(Ban(bans, kIronArmor, plain.variant));
    REQUIRE(bans.size() == 2);
    // Unbanning one name leaves the other banned.
    REQUIRE(Unban(bans, kIronArmor, plain.variant));
    CHECK_FALSE(IsBanned(bans, plain));
    CHECK(IsBanned(bans, enchanted));
    // Banning every copy folds the names' bans into one; a name's ban
    // beside it is then nothing new, and unbanning every copy clears all.
    REQUIRE(Ban(bans, kIronArmor, std::nullopt));
    REQUIRE(bans.size() == 1);
    REQUIRE_FALSE(Ban(bans, kIronArmor, enchanted.variant));
    CHECK(IsBanned(bans, plain));
    REQUIRE(Unban(bans, kIronArmor, std::nullopt));
    REQUIRE(bans.empty());
    REQUIRE_FALSE(Unban(bans, kIronArmor, enchanted.variant));
}

TEST_CASE("a pin names one variant of a form, and the other variants are other things", "[pins]")
{
    Holdable plain = Armour(kIronArmor, 0x4);
    plain.variant = ItemVariant{};
    Holdable enchanted = plain;
    enchanted.variant->enchantment = kFrost;
    Holdable whichever = plain;
    whichever.variant = std::nullopt;

    // Pinning the enchanted one: the plain one is not pinned, is set
    // aside for the slot it shares, and the pin is named as why.
    std::vector<Pin> pins;
    CHECK(ApplyRequest(pins, PinRequest::Pin, enchanted, Hand::None, false, true).empty());
    REQUIRE(pins.size() == 1);
    CHECK(FindPin(pins, enchanted) != nullptr);
    CHECK(FindPin(pins, plain) == nullptr);
    CHECK(FindPin(pins, whichever) != nullptr); // by form, whichever copy: the pinned one answers
    CHECK(SetAside(pins, plain));
    CHECK_FALSE(SetAside(pins, enchanted));
    const auto why = Shadowing(pins, plain);
    REQUIRE(why.size() == 1);
    CHECK(why[0].thing.variant->enchantment == kFrost);

    // The engine's equip of the plain one into the slot is refused for the
    // pin; of the enchanted one, passes; of the form with no name -- the
    // AI's entry -- passes as the pinned thing.
    CHECK(RefusesEngineEquip(pins, {}, plain, Hand::None, true).why == Refusal::Why::Conflict);
    CHECK_FALSE(RefusesEngineEquip(pins, {}, enchanted, Hand::None, true));
    CHECK_FALSE(RefusesEngineEquip(pins, {}, whichever, Hand::None, true));

    // Pinning the plain one instead displaces the enchanted one's pin,
    // and the displacement names what gave way.
    const auto displaced = ApplyRequest(pins, PinRequest::Pin, plain, Hand::None, false, true);
    REQUIRE(displaced.size() == 1);
    CHECK(displaced[0].form == kIronArmor);
    CHECK(displaced[0].variant->enchantment == kFrost);
    REQUIRE(pins.size() == 1);
    CHECK(pins[0].thing.variant->IsPlain());

    // A pin on the form, whichever copy; the panel then pinning a name
    // narrows it rather than standing a second pin beside it.
    pins.clear();
    (void)ApplyRequest(pins, PinRequest::Pin, whichever, Hand::None, false, true);
    (void)ApplyRequest(pins, PinRequest::Pin, enchanted, Hand::None, false, true);
    REQUIRE(pins.size() == 1);
    CHECK(pins[0].thing.variant->enchantment == kFrost);

    // Letting go names the name: the plain one's release leaves the
    // enchanted one's pin standing.
    pins = {{plain, Hand::None}, {enchanted, Hand::None}};
    CHECK(LetGo(pins, plain, Hand::None) == Hand::None);
    REQUIRE(pins.size() == 1);
    CHECK(pins[0].thing.variant->enchantment == kFrost);
}

TEST_CASE("a variant is what the player chose: tempering, an enchantment, a label, a theft", "[pins]")
{
    Holdable plain = Thing(kSteelDagger, Grip::Either);
    plain.kind = Kind::Weapon;
    plain.variant = ItemVariant{};
    Holdable tempered = plain;
    tempered.variant->tempering = 1.2f;
    Holdable renamed = plain;
    renamed.variant->label = "Fang";
    Holdable stolen = plain;
    stolen.variant->stolen = true;
    Holdable whichever = plain;
    whichever.variant = std::nullopt;

    // Each part makes another name; the parts together are one name.
    CHECK_FALSE(SameThing(plain, tempered));
    CHECK_FALSE(SameThing(plain, renamed));
    CHECK_FALSE(SameThing(plain, stolen));
    CHECK_FALSE(SameThing(tempered, renamed));
    CHECK(SameThing(whichever, plain));
    CHECK(SameThing(whichever, tempered));
    // Tempering read off a list and written to a save as a number comes
    // back within a hair, and is the same name.
    Holdable again = tempered;
    again.variant->tempering = 1.2000001f;
    CHECK(SameThing(tempered, again));
    Holdable more = tempered;
    more.variant->tempering = 1.3f;
    CHECK_FALSE(SameThing(tempered, more));

    // A pin on the tempered one: the plain one into its hand is another
    // thing and refused; a ban on the plain name leaves it alone.
    const std::vector<Pin> pins{{tempered, Hand::Right}};
    CHECK(RefusesEngineEquip(pins, {}, plain, Hand::Right, true).why == Refusal::Why::Conflict);
    CHECK_FALSE(RefusesEngineEquip(pins, {}, tempered, Hand::Right, true));
    Bans bans;
    REQUIRE(Ban(bans, kSteelDagger, plain.variant));
    CHECK(IsBanned(bans, plain));
    CHECK_FALSE(IsBanned(bans, tempered));
    CHECK(IsBanned(bans, whichever));
}

TEST_CASE("an enchantment is its effects at their strengths, in any order", "[pins]")
{
    // Resist Fire 25% and Resist Fire 50% are two enchantments the game
    // mints as two forms; the forms are the save's, so the name holds the
    // recipe instead, and reads the strength the enchanter set.
    constexpr std::uint32_t kResistFire = 0x581F7;
    constexpr std::uint32_t kFortifyHealth = 0x49507;
    ItemVariant weak;
    weak.enchantment = {{kResistFire, 25.0f, 0, 0}};
    ItemVariant strong;
    strong.enchantment = {{kResistFire, 50.0f, 0, 0}};
    CHECK_FALSE(SameVariant(weak, strong));
    CHECK(SameVariant(weak, weak));
    // Within a hair of the number the save wrote is the same strength.
    ItemVariant back = weak;
    back.enchantment[0].magnitude = 25.0001f;
    CHECK(SameVariant(weak, back));

    // Two effects are one enchantment whichever is written first, and one
    // of them alone is another.
    ItemVariant both;
    both.enchantment = {{kResistFire, 25.0f, 0, 0}, {kFortifyHealth, 30.0f, 0, 0}};
    ItemVariant swapped;
    swapped.enchantment = {{kFortifyHealth, 30.0f, 0, 0}, {kResistFire, 25.0f, 0, 0}};
    CHECK(SameVariant(both, swapped));
    CHECK_FALSE(SameVariant(both, weak));
    CHECK_FALSE(SameVariant(weak, both));
    // The same effect at two strengths is not the pair at one.
    ItemVariant doubled;
    doubled.enchantment = {{kResistFire, 25.0f, 0, 0}, {kResistFire, 50.0f, 0, 0}};
    CHECK_FALSE(SameVariant(doubled, both));
    // Duration tells a Soul Trap of five seconds from one of ten.
    ItemVariant brief;
    brief.enchantment = {{0x5B451, 0.0f, 5, 0}};
    ItemVariant lasting;
    lasting.enchantment = {{0x5B451, 0.0f, 10, 0}};
    CHECK_FALSE(SameVariant(brief, lasting));

    // A pin on the weak one refuses the strong one into its hand; a ban on
    // the strong one leaves the weak one alone.
    Holdable weakSword = Thing(kSteelDagger, Grip::Either);
    weakSword.kind = Kind::Weapon;
    weakSword.variant = weak;
    Holdable strongSword = weakSword;
    strongSword.variant = strong;
    const std::vector<Pin> pins{{weakSword, Hand::Right}};
    CHECK(RefusesEngineEquip(pins, {}, strongSword, Hand::Right, true).why == Refusal::Why::Conflict);
    CHECK_FALSE(RefusesEngineEquip(pins, {}, weakSword, Hand::Right, true));
    Bans bans;
    REQUIRE(Ban(bans, kSteelDagger, strong));
    CHECK(IsBanned(bans, strongSword));
    CHECK_FALSE(IsBanned(bans, weakSword));
}

TEST_CASE("the combat AI's score: a ban zeroes the form unless a pin holds it; one row allowed keeps the score",
          "[pins]")
{
    // The AI's entry is by form, whichever variant; the bag has the plain
    // stack, three copies.
    Holdable dagger = Thing(kSteelDagger, Grip::Either);
    dagger.kind = Kind::Weapon;
    dagger.count = 3;
    const ItemVariant plain;
    ItemVariant tempered;
    tempered.tempering = 1.2f;
    const std::vector<ItemVariant> plainOnly{plain};

    // Banned by its one row: zeroed.
    Bans bans;
    REQUIRE(Ban(bans, kSteelDagger, plain));
    REQUIRE(ShadowOf({}, bans, dagger, Hand::Right, plainOnly, false) == Shadow::Banned);
    // A tempered row beside it that no ban names: the form keeps its
    // score, and the detour picks the row.
    REQUIRE(ShadowOf({}, bans, dagger, Hand::Right, {plain, tempered}, false) == Shadow::None);
    // A ban on the form: every row, whatever rows there are.
    Bans whole;
    REQUIRE(Ban(whole, kSteelDagger));
    REQUIRE(ShadowOf({}, whole, dagger, Hand::Right, {plain, tempered}, false) == Shadow::Banned);
    // A spell has no rows: a ban on it is a ban on the form.
    Holdable flames = Thing(kFlames, Grip::Either);
    flames.kind = Kind::Spell;
    flames.count = 2;
    Bans spellBan;
    REQUIRE(Ban(spellBan, kFlames));
    REQUIRE(ShadowOf({}, spellBan, flames, Hand::Left, {}, false) == Shadow::Banned);
    REQUIRE(ShadowOf({}, bans, flames, Hand::Left, {}, false) == Shadow::None);

    // A rule pins the banned dagger for the fight: the pin is the override,
    // and the dagger is scored as any weapon -- or the follower stands
    // holding it and never swings (2026-09-12).
    Holdable pinnedDagger = dagger;
    pinnedDagger.variant = plain;
    const std::vector<Pin> pins{{pinnedDagger, Hand::Right}};
    REQUIRE(ShadowOf(pins, bans, dagger, Hand::Right, plainOnly, false) == Shadow::None);
    REQUIRE(ShadowOf(pins, whole, dagger, Hand::Right, plainOnly, false) == Shadow::None);
    // The pinned hand's own entry passes; the other hand's entry for the
    // only copy, already in the pinned hand, cannot be honoured.
    Holdable one = dagger;
    one.count = 1;
    REQUIRE(ShadowOf(pins, {}, one, Hand::Left, plainOnly, true) == Shadow::OnlyOneInOtherHand);
    REQUIRE(ShadowOf(pins, {}, dagger, Hand::Left, plainOnly, true) == Shadow::None);
    // A pin on another thing holding the entry's hand keeps the entry out.
    Holdable sword = Thing(kIronSword, Grip::Either);
    sword.kind = Kind::Weapon;
    const std::vector<Pin> swordPin{{sword, Hand::Right}};
    REQUIRE(ShadowOf(swordPin, {}, dagger, Hand::Right, plainOnly, false) == Shadow::PinnedAgainst);
    REQUIRE(ShadowOf(swordPin, {}, dagger, Hand::Left, plainOnly, false) == Shadow::None);
}

TEST_CASE("an equip naming no list takes the engine's pick, minus the banned variants", "[pins]")
{
    // Frea's bag, in the entry's order: an outfit dagger on a list with
    // only a worn mark (plain to the engine, and worn), the listless spare,
    // the dagger they were handed, poisoned (plain by name: the dose is not
    // part of it, though the engine's table calls its list distinct), the
    // tempered one.
    constexpr std::uint32_t kDaggerForm = 0x1397E;
    ItemVariant plain;
    ItemVariant tempered;
    tempered.tempering = 1.2f;
    const std::vector<VariantInBag> bag{
        {plain, true, true},      // 0: the outfit's, worn
        {plain, true, false},     // 1: the listless spare
        {plain, false, false},    // 2: poisoned, handed over
        {tempered, false, false}, // 3: tempered
    };
    Bans bans;

    // Nothing banned: the engine's own first choice, a plain copy that is
    // not worn.
    REQUIRE(EnginePick(bag, bans, kDaggerForm) == 1);
    // The plain name banned: every plain row goes, poisoned or not; the
    // tempered one is what is left.
    REQUIRE(Ban(bans, kDaggerForm, plain));
    REQUIRE(EnginePick(bag, bans, kDaggerForm) == 3);
    // And the tempered name too: nothing to hand over.
    REQUIRE(Ban(bans, kDaggerForm, tempered));
    REQUIRE_FALSE(EnginePick(bag, bans, kDaggerForm).has_value());
    // A ban on the form is every copy at once.
    bans.clear();
    REQUIRE(Ban(bans, kDaggerForm, std::nullopt));
    REQUIRE_FALSE(EnginePick(bag, bans, kDaggerForm).has_value());

    // No plain copy: the entry's order decides, and a worn copy is passed
    // over -- it is in a hand already, and the engine does the same.
    bans.clear();
    ItemVariant enchanted;
    enchanted.enchantment = kFrost;
    const std::vector<VariantInBag> named{{tempered, false, true}, {enchanted, false, false}, {tempered, false, false}};
    REQUIRE(EnginePick(named, bans, kDaggerForm) == 1);
    REQUIRE(Ban(bans, kDaggerForm, enchanted));
    REQUIRE(EnginePick(named, bans, kDaggerForm) == 2);
    // All worn but the banned one: the request goes on as it came.
    const std::vector<VariantInBag> held{{tempered, false, true}, {enchanted, false, false}};
    REQUIRE_FALSE(EnginePick(held, bans, kDaggerForm).has_value());
    // A ban on another form is nobody's business here.
    bans.clear();
    REQUIRE(Ban(bans, 0x13989, enchanted));
    REQUIRE(EnginePick(named, bans, kDaggerForm) == 1);
}

TEST_CASE("two variants of one weapon are two pins, one in each hand", "[pins]")
{
    // A plain steel dagger and a tempered one: pinned left and right,
    // they are two things, and each hand's pin stands on its own.
    Holdable plain = Thing(kSteelDagger, Grip::Either);
    plain.kind = Kind::Weapon;
    plain.variant = ItemVariant{};
    plain.count = 1; // one copy of each VARIANT
    Holdable tempered = plain;
    tempered.variant->tempering = 1.2f;

    std::vector<Pin> pins;
    (void)ApplyRequest(pins, PinRequest::Pin, plain, Hand::Left, false, true);
    (void)ApplyRequest(pins, PinRequest::Pin, tempered, Hand::Right, false, true);
    REQUIRE(pins.size() == 2);
    CHECK(pins[0].hands == Hand::Left);
    CHECK(pins[1].hands == Hand::Right);
    // Each is refused the other's hand: one copy of the name cannot be in
    // two hands, and the other hand's pin is another name.
    CHECK(RefusesEngineEquip(pins, {}, plain, Hand::Right, true).why == Refusal::Why::OneCopy);
    CHECK_FALSE(RefusesEngineEquip(pins, {}, plain, Hand::Left, true));
    CHECK_FALSE(RefusesEngineEquip(pins, {}, tempered, Hand::Right, true));
    CHECK(RefusesEngineEquip(pins, {}, tempered, Hand::Left, true).why == Refusal::Why::OneCopy);

    // The plain dagger alone pinned right, and two of the name in the bag:
    // the second may go left, the tempered one may go left, and the AI's
    // entry, by form with no name, passes too.
    pins = {{plain, Hand::Right}};
    Holdable spare = plain;
    spare.count = 2;
    CHECK_FALSE(RefusesEngineEquip(pins, {}, spare, Hand::Left, true));
    CHECK_FALSE(RefusesEngineEquip(pins, {}, tempered, Hand::Left, true));
    CHECK(RefusesEngineEquip(pins, {}, plain, Hand::Left, true).why == Refusal::Why::OneCopy);
    Holdable whichever = spare;
    whichever.variant = std::nullopt;
    CHECK_FALSE(RefusesEngineEquip(pins, {}, whichever, Hand::Left, true));

    // After the fight, a pin on the other name is not the pin from before.
    const AfterFight settle = SettleAfterFight({{tempered, Hand::Left}}, {{plain, Hand::Left}});
    REQUIRE(settle.released.size() == 1);
    CHECK(settle.released[0].variant->tempering == 1.2f);
    CHECK(settle.released[0].takeOff);
    REQUIRE(settle.restored.size() == 1);
    CHECK(settle.restored[0].thing.variant->IsPlain());
}

TEST_CASE("the engine's equip is refused by one rule, with the reason", "[pins]")
{
    // A dagger pinned right, a spell pinned left.
    Holdable dagger = Thing(kIronDagger, Grip::Either);
    const Holdable flames = Thing(kFlames, Grip::Either);
    const std::vector<Pin> pins{{dagger, Hand::Right}, {flames, Hand::Left}};

    // Its own hand passes; the other is refused for the one copy, and with
    // a second copy for the pin that holds that hand.
    CHECK_FALSE(RefusesEngineEquip(pins, {}, dagger, Hand::Right, true));
    CHECK(RefusesEngineEquip(pins, {}, dagger, Hand::Left, true).why == Refusal::Why::OneCopy);
    dagger.count = 2;
    const Refusal other = RefusesEngineEquip(pins, {}, dagger, Hand::Left, true);
    CHECK(other.why == Refusal::Why::OtherPin);
    CHECK(other.pin->thing.form == kIronDagger);
    // No hand named for a pinned thing: the engine's choice, not refused.
    CHECK_FALSE(RefusesEngineEquip(pins, {}, dagger, Hand::None, true));

    // Another thing into a pinned hand: refused, and the pin in the way
    // named. Into a free place, or with no hand or slot at all, not.
    const Refusal sword = RefusesEngineEquip(pins, {}, Thing(kIronSword, Grip::Either), Hand::Right, true);
    CHECK(sword.why == Refusal::Why::Conflict);
    CHECK(sword.pin->thing.form == kIronDagger);
    CHECK(RefusesEngineEquip(pins, {}, Thing(kHuntingBow, Grip::Both), Hand::Both, true));
    Holdable potion;
    potion.form = 0x3EADE;
    CHECK_FALSE(RefusesEngineEquip(pins, {}, potion, Hand::None, true));
    CHECK_FALSE(RefusesEngineEquip({}, {}, dagger, Hand::Left, true));
}

TEST_CASE("a request from the panel does the same to either book", "[pins]")
{
    const Holdable dagger = Thing(kIronDagger, Grip::Either);
    const Holdable sword = Thing(kIronSword, Grip::Either);
    std::vector<Pin> book{{dagger, Hand::Right}};

    // A pin makes room: the dagger's pin gives way to the sword's.
    const auto displaced = ApplyRequest(book, PinRequest::Pin, sword, Hand::Right, false, true);
    REQUIRE(displaced.size() == 1);
    CHECK(displaced[0].form == kIronDagger);
    REQUIRE(book.size() == 1);
    CHECK(book[0].thing.form == kIronSword);

    // An equip makes room too, and pins nothing; the only copy moving
    // across leaves the hand it came from.
    book = {{dagger, Hand::Right}};
    CHECK(ApplyRequest(book, PinRequest::Equip, sword, Hand::Right, false, true).size() == 1);
    CHECK(book.empty());
    book = {{dagger, Hand::Left}};
    CHECK(ApplyRequest(book, PinRequest::Equip, dagger, Hand::Right, true, true).empty());
    CHECK(book.empty());

    // A ban lets the whole pin go.
    book = {{dagger, Hand::Both}};
    CHECK(ApplyRequest(book, PinRequest::Ban, dagger, Hand::None, false, true).empty());
    CHECK(book.empty());
}

TEST_CASE("the engine's spell and shout equips are refused as an item's are; ours pass", "[pins]")
{
    // Flames pinned left; the AI, or a mod's script, wants a spell in a
    // hand: the right is free, the left is pinned.
    World w;
    const Holdable flames = Thing(kFlames, Grip::Either);
    const Holdable firebolt = Thing(kFirebolt, Grip::RightOnly);
    const Holdable bolt = Thing(kLightningBolt, Grip::LeftOnly);
    w.pins = {{flames, Hand::Left}};
    w.OurEquip(flames, Hand::Left);
    CHECK(w.EngineEquip(firebolt, Hand::Right));
    CHECK(w.hands.right == kFirebolt);
    CHECK_FALSE(w.EngineEquip(bolt, Hand::Left));
    CHECK(w.hands.left == kFlames);
    // The spell our own record casts takes the pinned hand: the package's
    // equip is ours, and the watchdog puts the pin back after.
    w.OurCast(bolt, Hand::Left);
    CHECK(w.hands.left == kLightningBolt);
    w.Watchdog();
    CHECK(w.hands.left == kFlames);

    // A banned spell is refused into any hand, pins or none, with the
    // reason; the same spell cast by a rule of ours still goes.
    w.bans = {{kFirebolt, {}}};
    CHECK_FALSE(w.EngineEquip(firebolt, Hand::Right));
    CHECK(RefusesEngineEquip({}, w.bans, firebolt, Hand::Right, true).why == Refusal::Why::Banned);
    CHECK(RefusesEngineEquip({}, w.bans, firebolt, Hand::None, true).why == Refusal::Why::Banned);
    w.OurCast(firebolt, Hand::Right);
    CHECK(w.hands.right == kFirebolt);

    // A shout or a power has no hand: banned, it is refused; and a voice
    // pin holds the voice against another one.
    Holdable battleCry;
    battleCry.form = 0x000E40C3;
    battleCry.kind = Kind::Voice;
    Holdable unrelentingForce;
    unrelentingForce.form = 0x00013E07;
    unrelentingForce.kind = Kind::Voice;
    const Bans banned{{unrelentingForce.form, {}}};
    CHECK(RefusesEngineEquip({}, banned, unrelentingForce, Hand::None, true).why == Refusal::Why::Banned);
    CHECK_FALSE(RefusesEngineEquip({}, banned, battleCry, Hand::None, true));
    const std::vector<Pin> voicePin{{battleCry, Hand::None}};
    CHECK(RefusesEngineEquip(voicePin, {}, unrelentingForce, Hand::None, true).why == Refusal::Why::Conflict);
    CHECK_FALSE(RefusesEngineEquip(voicePin, {}, battleCry, Hand::None, true));
}
