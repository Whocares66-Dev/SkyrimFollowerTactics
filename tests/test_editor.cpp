// The editor's questions of a rule: what the follower no longer has sets
// the rule aside, and a follower away sets it aside first. No Skyrim.

#include <catch2/catch_test_macros.hpp>

#include "Build.h"
#include "core/Editor.h"

using namespace ft;
using namespace ft::test;

namespace
{

constexpr std::uint32_t kFirebolt = 0x12FCD;
constexpr std::uint32_t kSword = 0x13989;
constexpr std::uint32_t kOtherFollower = 0x201;
constexpr std::uint32_t kAway = 0x202;

Holdings Bag()
{
    Holdings has;
    has.self = 0xA2C94;
    has.peers = {kOtherFollower};
    has.consumables.push_back({kHealthPotion, ConsumableKind::Potion, {"Restore Health"}});
    has.consumables.push_back({0x64B33, ConsumableKind::Food, {"Restore Stamina"}});
    has.castable = {kFirebolt};
    ItemVariant tempered;
    tempered.tempering = 1.2f;
    has.things = {{kSword, ItemVariant{}, Kind::Weapon},
                  {kFirebolt, std::nullopt, Kind::Spell},
                  {kSword, tempered, Kind::Weapon}};
    return has;
}

Rule With(Action action)
{
    Rule r = HealBelow(0.5f);
    r.actions = {std::move(action)};
    return r;
}

} // namespace

TEST_CASE("a policy is had while something carried of its kind has its effect", "[editor]")
{
    const Holdings has = Bag();
    Action drink;
    drink.kind = ActionKind::DrinkStrongest;
    drink.effect = "Restore Health";
    REQUIRE(ActionHad(drink, has));
    // The right effect on the wrong kind is not it: food restoring stamina
    // is no stamina potion.
    drink.effect = "Restore Stamina";
    REQUIRE_FALSE(ActionHad(drink, has));
    Action eat;
    eat.kind = ActionKind::EatWeakestFood;
    eat.effect = "Restore Stamina";
    REQUIRE(ActionHad(eat, has));
    // Nothing carried with the effect: the potion drunk up.
    drink.effect = "Fortify Destruction Power";
    REQUIRE_FALSE(ActionHad(drink, has));
    REQUIRE(RuleSetAside(With(drink), has) == Aside::NotHad);
}

TEST_CASE("an 'any' rule is had while something carried is worth rolling", "[editor]")
{
    Holdings has = Bag(); // a health potion and a food, neither a buff

    Action drinkAny;
    drinkAny.kind = ActionKind::DrinkAny;
    // A policy with no effect named is the other "any", and asks the same.
    Action strongestAny;
    strongestAny.kind = ActionKind::DrinkStrongest;

    // Six health potions answer "Restore Health" and answer nothing at all
    // for a rule that wants a buff.
    REQUIRE_FALSE(ActionHad(drinkAny, has));
    REQUIRE_FALSE(ActionHad(strongestAny, has));
    REQUIRE(RuleSetAside(With(drinkAny), has) == Aside::NotHad);

    // One fortify in the bag and both are had.
    has.consumables.push_back({0x301, ConsumableKind::Potion, {"Fortify One-handed"}, true});
    REQUIRE(ActionHad(drinkAny, has));
    REQUIRE(ActionHad(strongestAny, has));

    // It is the kind's own bag that is asked: a buff FOOD is no buff potion.
    Action eatAny;
    eatAny.kind = ActionKind::EatStrongestFood;
    REQUIRE_FALSE(ActionHad(eatAny, has));
    has.consumables.push_back({0x302, ConsumableKind::Food, {"Fortify Health"}, true});
    REQUIRE(ActionHad(eatAny, has));

    // A poison names no form and is had regardless, as every apply rule is:
    // whether a weapon in hand takes one is the evaluator's question, not
    // the editor's.
    Action applyAny;
    applyAny.kind = ActionKind::ApplyAny;
    REQUIRE(ActionHad(applyAny, has));
}

TEST_CASE("a named thing is had by form and kind; a form of none always is", "[editor]")
{
    const Holdings has = Bag();
    Action potion;
    potion.kind = ActionKind::DrinkPotion;
    potion.form = kHealthPotion;
    REQUIRE(ActionHad(potion, has));
    potion.form = 0x3EAE1;
    REQUIRE_FALSE(ActionHad(potion, has));
    // The health potion under eat-food: the wrong kind, not had.
    Action food;
    food.kind = ActionKind::EatFood;
    food.form = kHealthPotion;
    REQUIRE_FALSE(ActionHad(food, has));

    Action cast;
    cast.kind = ActionKind::CastSpell;
    cast.form = kFirebolt;
    REQUIRE(ActionHad(cast, has));
    cast.form = 0x2F3B8;
    REQUIRE_FALSE(ActionHad(cast, has));

    Action equip;
    equip.kind = ActionKind::EquipWeapon;
    equip.form = kSword;
    REQUIRE(ActionHad(equip, has));
    equip.form = 0x13990;
    REQUIRE_FALSE(ActionHad(equip, has));
    equip.form = 0; // let go of every weapon pin: names nothing
    REQUIRE(ActionHad(equip, has));
    // A variant named is had while a row of it is: the smithed sword and
    // not a tempering nobody carries; the form alone, by any row.
    equip.form = kSword;
    equip.variant.emplace().tempering = 1.2f;
    REQUIRE(ActionHad(equip, has));
    equip.variant->tempering = 1.5f;
    REQUIRE_FALSE(ActionHad(equip, has));
    equip.variant = ItemVariant{}; // the plain stack is a row of its own
    REQUIRE(ActionHad(equip, has));
    equip.variant = std::nullopt; // the form: whichever
    REQUIRE(ActionHad(equip, has));

    Action attack;
    attack.kind = ActionKind::Attack;
    REQUIRE(ActionHad(attack, has));
}

TEST_CASE("what the settings require is part of what a follower has", "[editor]")
{
    Holdings has;
    has.castable.push_back(0x111);

    // A blow names no form, so nothing else would ever set it aside.
    Action bash;
    bash.kind = ActionKind::PowerBash;
    REQUIRE(ActionHad(bash, has)); // no perk asked for: the default
    has.powerBashPerk = false;
    REQUIRE_FALSE(ActionHad(bash, has));

    // A plain cast is asked only whether the spell is known; a dual cast is
    // asked whether it is one they may cast from both hands.
    Action cast;
    cast.kind = ActionKind::CastSpell;
    cast.form = 0x111;
    REQUIRE(ActionHad(cast, has));
    cast.dual = true;
    REQUIRE_FALSE(ActionHad(cast, has));
    has.dualCastable.push_back(0x111);
    REQUIRE(ActionHad(cast, has));
}

TEST_CASE("a follower away sets the rule aside, and is said before what is not had", "[editor]")
{
    const Holdings has = Bag();
    Rule r = HealBelow(0.5f);
    REQUIRE(RuleSetAside(r, has) == Aside::None);

    r.subject = SubjectKind::Follower;
    r.subjectForm = kOtherFollower;
    REQUIRE(ConditionHad(r, has));
    r.subjectForm = kAway;
    REQUIRE_FALSE(ConditionHad(r, has));
    REQUIRE(RuleSetAside(r, has) == Aside::FollowerAway);

    // The party member of Attacking: the player and the follower themself
    // are always with us; another follower has to be.
    r.subject = SubjectKind::Enemy;
    r.predicate = PredicateKind::Attacking;
    r.subjectForm = 0;
    REQUIRE(ConditionHad(r, has));
    r.subjectForm = has.self;
    REQUIRE(ConditionHad(r, has));
    r.subjectForm = kAway;
    REQUIRE_FALSE(ConditionHad(r, has));

    Rule aimed = HealBelow(0.5f);
    aimed.actionTarget = ActionTargetKind::Follower;
    aimed.actionTargetForm = kOtherFollower;
    REQUIRE(TargetHad(aimed, has));
    aimed.actionTargetForm = kAway;
    REQUIRE_FALSE(TargetHad(aimed, has));
    aimed.FirstAction().effect = "Fortify Destruction Power"; // not had either
    REQUIRE(RuleSetAside(aimed, has) == Aside::FollowerAway);
}
