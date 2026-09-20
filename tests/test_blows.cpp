// The rules of a swing and a bash, from what the hands hold: vanilla's,
// which the game side maps to the race's attack events.
#include "core/Blows.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ft;

TEST_CASE("a power attack is the right hand's blade, else the left's, else the fists", "[blows]")
{
    REQUIRE(SwingWith({Held::OneHander, Held::Nothing}) == Swing::Right);
    REQUIRE(SwingWith({Held::TwoHander, Held::Nothing}) == Swing::Right);
    REQUIRE(SwingWith({Held::OneHander, Held::Shield}) == Swing::Right);
    REQUIRE(SwingWith({Held::OneHander, Held::Spell}) == Swing::Right);
    REQUIRE(SwingWith({Held::OneHander, Held::OneHander}) == Swing::Both);
    // The right hand swinging nothing, the left's blade swings, whatever the
    // right holds.
    REQUIRE(SwingWith({Held::Nothing, Held::OneHander}) == Swing::Left);
    REQUIRE(SwingWith({Held::Spell, Held::OneHander}) == Swing::Left);
    REQUIRE(SwingWith({Held::Staff, Held::OneHander}) == Swing::Left);
    REQUIRE(SwingWith({Held::Nothing, Held::Nothing}) == Swing::Fists);
    // A bow, a staff, a spell or a shield swings nothing, and a hand holding
    // one is not a fist.
    REQUIRE(SwingWith({Held::Bow, Held::Nothing}) == Swing::None);
    REQUIRE(SwingWith({Held::Staff, Held::Nothing}) == Swing::None);
    REQUIRE(SwingWith({Held::Spell, Held::Shield}) == Swing::None);
    REQUIRE(SwingWith({Held::Spell, Held::Spell}) == Swing::None);
    REQUIRE(SwingWith({Held::Nothing, Held::Shield}) == Swing::None);
    REQUIRE(SwingWith({Held::Nothing, Held::Staff}) == Swing::None);
}

TEST_CASE("a bash is what blocks: a shield or torch, or the right hand's weapon with the left empty", "[blows]")
{
    REQUIRE(BashesWith({Held::Spell, Held::Shield}));
    REQUIRE(BashesWith({Held::OneHander, Held::Shield}));
    REQUIRE(BashesWith({Held::OneHander, Held::Torch}));
    REQUIRE(BashesWith({Held::OneHander, Held::Nothing}));
    REQUIRE(BashesWith({Held::TwoHander, Held::Nothing}));
    REQUIRE(BashesWith({Held::Bow, Held::Nothing}));
    REQUIRE(BashesWith({Held::Staff, Held::Nothing}));
    // A weapon alone in the left hand cannot block, nor two hands each
    // holding something, nor the fists, nor a spell hand.
    REQUIRE_FALSE(BashesWith({Held::Nothing, Held::Staff}));
    REQUIRE_FALSE(BashesWith({Held::Nothing, Held::OneHander}));
    REQUIRE_FALSE(BashesWith({Held::OneHander, Held::OneHander}));
    REQUIRE_FALSE(BashesWith({Held::OneHander, Held::Staff}));
    REQUIRE_FALSE(BashesWith({Held::OneHander, Held::Spell}));
    REQUIRE_FALSE(BashesWith({Held::Staff, Held::Staff}));
    REQUIRE_FALSE(BashesWith({Held::Nothing, Held::Nothing}));
    REQUIRE_FALSE(BashesWith({Held::Spell, Held::Nothing}));
    REQUIRE_FALSE(BashesWith({Held::Spell, Held::Spell}));
}

TEST_CASE("the attack events, the costs, the hand a poison goes on, the charge after a gem", "[blows]")
{
    using namespace ft;
    REQUIRE(std::string(PowerAttackEvent(Swing::Both)) == "attackPowerStartDualWield");
    REQUIRE(std::string(PowerAttackEvent(Swing::Right)) == "attackPowerStartInPlace");
    REQUIRE(std::string(PowerAttackEvent(Swing::Fists)) == "attackPowerStartInPlace");
    REQUIRE(std::string(PowerAttackEvent(Swing::Left)) == "attackPowerStartInPlaceLeftHand");
    REQUIRE(PowerAttackEvent(Swing::None) == nullptr);
    REQUIRE(std::string(BashEvent(false)) == "bashStart");
    REQUIRE(std::string(BashEvent(true)) == "bashPowerStart");

    // Vanilla's settings: a 10-weight sword costs (10 * 1 + 20) * 2 = 60
    // before the perks; the fists 42.
    REQUIRE(PowerAttackStamina(10.0f, 1.0f, 20.0f, 2.0f) == 60.0f);
    REQUIRE(PowerAttackStamina(1.0f, 1.0f, 20.0f, 2.0f) == 42.0f);

    // The right hand's clean weapon before the left's; a poisoned right
    // hand sends the dose left; a staff takes none.
    REQUIRE(HandToPoison(true, false, true, false) == Hand::Right);
    REQUIRE(HandToPoison(true, true, true, false) == Hand::Left);
    REQUIRE(HandToPoison(true, true, true, true) == Hand::None);
    REQUIRE(HandToPoison(false, false, true, false) == Hand::Left);
    REQUIRE(HandToPoison(false, false, false, false) == Hand::None);

    REQUIRE(ChargeAfterRecharge(10.0f, 100.0f, 50.0f) == 60.0f);
    REQUIRE(ChargeAfterRecharge(80.0f, 100.0f, 50.0f) == 100.0f);
    REQUIRE(ChargeAfterRecharge(10.0f, 100.0f, -5.0f) == 10.0f);
    REQUIRE(ChargeAfterRecharge(100.0f, 100.0f, 0.0f) == 100.0f);
}

TEST_CASE("a poison goes on the worn copy, then the vial is spent", "[blows]")
{
    using namespace ft;
    REQUIRE(PlanPoison(true, true) ==
            std::vector<ItemStep>{ItemStep::WriteDose, ItemStep::SpendPoison, ItemStep::PlaySound});
    // No weapon that takes one, or no copy of it worn: nothing is done,
    // and the vial is not spent.
    REQUIRE(PlanPoison(false, true).empty());
    REQUIRE(PlanPoison(true, false).empty());
}

TEST_CASE("a soul is written, the weapon refreshed, and only then the gem spent", "[blows]")
{
    using namespace ft;
    REQUIRE(PlanRecharge(true, true, true, false) == std::vector<ItemStep>{ItemStep::WriteCharge,
                                                                           ItemStep::RefreshAbility, ItemStep::SpendGem,
                                                                           ItemStep::PlaySound});
    // A reusable gem is emptied where an ordinary one is taken; the order
    // is otherwise the same.
    REQUIRE(PlanRecharge(true, true, true, true) == std::vector<ItemStep>{ItemStep::WriteCharge,
                                                                          ItemStep::RefreshAbility, ItemStep::EmptyGem,
                                                                          ItemStep::PlaySound});
    // Nothing in need of a charge, no gem, or no worn copy to write to:
    // nothing is done, and no gem is spent.
    REQUIRE(PlanRecharge(false, true, true, false).empty());
    REQUIRE(PlanRecharge(true, false, true, false).empty());
    REQUIRE(PlanRecharge(true, true, false, false).empty());
}
