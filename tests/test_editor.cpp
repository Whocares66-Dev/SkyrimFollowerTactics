// The editor's questions of a rule: what the follower no longer has sets
// the rule aside, and a follower away sets it aside first. No Skyrim.

#include <catch2/catch_test_macros.hpp>

#include "Build.h"
#include "core/Editor.h"
#include "core/Vocabulary.h"

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
    // The outright food roll asks the same question the food policy does.
    Action eatAny;
    eatAny.kind = ActionKind::EatStrongestFood;
    Action eatAnyFood;
    eatAnyFood.kind = ActionKind::EatAnyFood;
    REQUIRE_FALSE(ActionHad(eatAny, has));
    REQUIRE_FALSE(ActionHad(eatAnyFood, has));
    has.consumables.push_back({0x302, ConsumableKind::Food, {"Fortify Health"}, true});
    REQUIRE(ActionHad(eatAny, has));
    REQUIRE(ActionHad(eatAnyFood, has));

    // A poison is asked of the bag exactly as a potion is. Whether a weapon
    // in hand can take one stays the evaluator's question -- that changes
    // from tick to tick -- but carrying one at all does not, and this read
    // "had regardless" until 2026-09-17, which showed an Apply rule as ready
    // with no poison in the bag (and a Charge rule with no gem).
    Action applyAny;
    applyAny.kind = ActionKind::ApplyAny;
    REQUIRE_FALSE(ActionHad(applyAny, has));
    has.consumables.push_back({0x303, ConsumableKind::Poison, {"Damage Health"}, true});
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

TEST_CASE("a rule is set aside only when none of its actions is had", "[editor]")
{
    const Holdings has = Bag(); // carries a health potion, knows Firebolt
    Action had;
    had.kind = ActionKind::DrinkStrongest;
    had.effect = "Restore Health";
    Action gone;
    gone.kind = ActionKind::DrinkStrongest;
    gone.effect = "Fortify Destruction Power";
    Action cast;
    cast.kind = ActionKind::CastSpell;
    cast.form = kFirebolt;

    Rule r = HealBelow(0.5f);

    // Three actions, one not had: the rule stands, one counted.
    r.actions = {had, gone, cast};
    REQUIRE(ActionsNotHad(r, has) == 1);
    REQUIRE(RuleSetAside(r, has) == Aside::None);

    // Every one gone: set aside.
    r.actions = {gone, gone};
    REQUIRE(ActionsNotHad(r, has) == 2);
    REQUIRE(RuleSetAside(r, has) == Aside::NotHad);

    // All had: none counted.
    r.actions = {had, cast};
    REQUIRE(ActionsNotHad(r, has) == 0);
    REQUIRE(RuleSetAside(r, has) == Aside::None);

    // No actions at all is not "none had".
    r.actions.clear();
    REQUIRE(ActionsNotHad(r, has) == 0);
    REQUIRE(RuleSetAside(r, has) == Aside::None);
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

TEST_CASE("a charge rule is had only with a gem to spend", "[editor]")
{
    // The bag from Bag() carries a potion and a food and no gem at all.
    Holdings has = Bag();
    Action strongest;
    strongest.kind = ActionKind::ChargeStrongestSoulGem;
    Action weakest;
    weakest.kind = ActionKind::ChargeWeakestSoulGem;
    Action named;
    named.kind = ActionKind::ChargeSoulGem;
    named.form = 0x2E4E2; // a petty gem

    REQUIRE_FALSE(ActionHad(strongest, has));
    REQUIRE_FALSE(ActionHad(weakest, has));
    REQUIRE_FALSE(ActionHad(named, has));
    REQUIRE(RuleSetAside(With(strongest), has) == Aside::NotHad);

    // The scan lists a gem only when it holds a soul, so one in the bag is
    // one that can be spent.
    has.consumables.push_back({0x2E4E2, ConsumableKind::SoulGem, {}});
    REQUIRE(ActionHad(strongest, has));
    REQUIRE(ActionHad(weakest, has));
    REQUIRE(ActionHad(named, has));
    REQUIRE(RuleSetAside(With(strongest), has) == Aside::None);

    // A gem of another form still answers the two sizes, which choose at
    // the firing, but not the rule that names this one.
    has.consumables.back().form = 0x2E4F4;
    REQUIRE(ActionHad(strongest, has));
    REQUIRE_FALSE(ActionHad(named, has));
}

TEST_CASE("a poison rule is had only with a poison to put on", "[editor]")
{
    Holdings has = Bag();
    Action strongest;
    strongest.kind = ActionKind::ApplyStrongest;
    strongest.effect = "Damage Health";
    Action any;
    any.kind = ActionKind::ApplyAny;
    Action rolled; // a policy with no effect: rolls one, then takes the strongest of it
    rolled.kind = ActionKind::ApplyWeakest;
    Action named;
    named.kind = ActionKind::ApplyPoison;
    named.form = 0x73F30;

    REQUIRE_FALSE(ActionHad(strongest, has));
    REQUIRE_FALSE(ActionHad(any, has));
    REQUIRE_FALSE(ActionHad(rolled, has));
    REQUIRE_FALSE(ActionHad(named, has));
    REQUIRE(RuleSetAside(With(any), has) == Aside::NotHad);

    // Every poison is worth rolling, so one in the bag answers the rolls;
    // the effect ones want that effect.
    Holdings::Consumable poison{0x73F30, ConsumableKind::Poison, {"Damage Health"}};
    poison.any = true;
    has.consumables.push_back(poison);
    REQUIRE(ActionHad(strongest, has));
    REQUIRE(ActionHad(any, has));
    REQUIRE(ActionHad(rolled, has));
    REQUIRE(ActionHad(named, has));
    REQUIRE(RuleSetAside(With(any), has) == Aside::None);

    // A poison of another effect still answers the rolls, and not the rule
    // that asks for Damage Health.
    has.consumables.back().effects = {"Damage Stamina"};
    REQUIRE_FALSE(ActionHad(strongest, has));
    REQUIRE(ActionHad(any, has));

    // A potion is not a poison, whatever its effect says.
    has.consumables.back().kind = ConsumableKind::Potion;
    REQUIRE_FALSE(ActionHad(any, has));
}

TEST_CASE("every action that chooses its thing needs something to choose", "[editor]")
{
    // The guard for the hole this file's two tests above came from: an
    // action that works out WHICH thing at the firing carries no form, so
    // it reaches ActionHad's "names nothing, so nothing to miss" line --
    // which is true of an Unequip and of a blow, and of nothing else. Every
    // such action must answer no with an empty bag, or a rule that can
    // never do anything reads as ready.
    const Holdings nothing;
    for (std::uint8_t i = 1; i < static_cast<std::uint8_t>(ActionKind::COUNT); ++i)
    {
        const auto kind = static_cast<ActionKind>(i);
        if (!ChoosesForm(kind))
            continue;
        Action a;
        a.kind = kind;
        INFO(WireName(kind));
        REQUIRE_FALSE(ActionHad(a, nothing));
    }
}

TEST_CASE("a rule takes the name its thing has now, and keeps the old one while it is away", "[editor]")
{
    RuleSet rules;
    rules.rules.push_back(test::HealBelow(0.5f));
    Rule equip;
    equip.FirstAction().kind = ActionKind::EquipWeapon;
    equip.FirstAction().form = 0x12EB7;
    equip.FirstAction().name = "Iron Dagger";
    rules.rules.push_back(equip);

    // The dagger tempered and renamed at the grindstone: the rule follows.
    const auto renamed = [](const Action &a) { return a.form == 0x12EB7 ? std::string("Iron Dagger (Fine)") : ""; };
    REQUIRE(RefreshActionNames(rules, renamed));
    REQUIRE(rules.rules[1].actions[0].name == "Iron Dagger (Fine)");
    // The same again: nothing to change.
    REQUIRE_FALSE(RefreshActionNames(rules, renamed));

    // The dagger sold: the rule keeps its last name.
    const auto gone = [](const Action &) { return std::string{}; };
    REQUIRE_FALSE(RefreshActionNames(rules, gone));
    REQUIRE(rules.rules[1].actions[0].name == "Iron Dagger (Fine)");

    // A policy names no form and is never asked.
    bool asked = false;
    const auto counting = [&](const Action &) {
        asked = true;
        return std::string("x");
    };
    RuleSet policy;
    policy.rules.push_back(test::HealBelow(0.5f));
    REQUIRE_FALSE(RefreshActionNames(policy, counting));
    REQUIRE_FALSE(asked);
}
