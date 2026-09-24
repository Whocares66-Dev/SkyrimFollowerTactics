// Would taking or casting a thing add anything (core/Snapshot.h,
// AnyWouldLand): the check itself, then each way a rule meets it -- a named
// food or potion, a cast on oneself, a cast on another, a policy -- through
// the evaluator.

#include <catch2/catch_test_macros.hpp>

#include "Build.h"
#include "core/Evaluator.h"

#include <vector>

using namespace ft;
using namespace ft::test;

namespace
{

constexpr std::uint32_t kGoatCheese = 0x00064B35;
constexpr std::uint32_t kTwoBoons = 0x000A0001;
constexpr std::uint32_t kWine = 0x000A0002;
constexpr std::uint32_t kOakflesh = 0x0005AD5C;

PotionStock::Effect Boon(const char *name, float magnitude, float duration = 60.0f)
{
    return {name, magnitude, duration, true, false};
}

PotionStock::Effect Bane(const char *name, float magnitude, float duration = 10.0f)
{
    return {name, magnitude, duration, false, true};
}

// IF self: any THEN <target>: <action>.
Rule Do(const Action &action, ActionTargetKind target = ActionTargetKind::Self)
{
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::Any;
    r.actionTarget = target;
    r.FirstAction() = action;
    return r;
}

Action Named(ActionKind kind, std::uint32_t form)
{
    Action a;
    a.kind = kind;
    a.form = form;
    return a;
}

Verdict VerdictOf(const Rule &rule, const Snapshot &s)
{
    RuleSet rs;
    rs.rules.push_back(rule);
    EvalContext ctx;
    Trace trace;
    (void)Evaluate(rs, s, ctx, &trace);
    return trace.at(0);
}

} // namespace

TEST_CASE("an effect lands unless one of its name is in force at least as strongly", "[landing]")
{
    const std::vector<RunningEffect> none;
    // Nothing lasting is not judged: the cooldown spaces it.
    REQUIRE(AnyWouldLand({}, none));
    REQUIRE(AnyWouldLand({}, {{"Fortify Health", 50.0f}}));

    const std::vector<RunningEffect> fortify{{"Fortify Health", 50.0f}};
    REQUIRE(AnyWouldLand(fortify, none));
    REQUIRE(AnyWouldLand(fortify, {{"Fortify Health", 49.0f}})); // a weaker dose: the stronger lands
    REQUIRE_FALSE(AnyWouldLand(fortify, {{"Fortify Health", 50.0f}}));
    REQUIRE_FALSE(AnyWouldLand(fortify, {{"Fortify Health", 80.0f}}));
    REQUIRE(AnyWouldLand(fortify, {{"Fortify Magicka", 80.0f}})); // another name is no cover

    // Several effects: any one landing is enough.
    const std::vector<RunningEffect> two{{"Fortify Health", 50.0f}, {"Resist Fire", 30.0f}};
    REQUIRE(AnyWouldLand(two, {{"Fortify Health", 50.0f}}));
    REQUIRE(AnyWouldLand(two, {{"Fortify Health", 50.0f}, {"Resist Fire", 20.0f}}));
    REQUIRE_FALSE(AnyWouldLand(two, {{"Fortify Health", 50.0f}, {"Resist Fire", 30.0f}}));
}

TEST_CASE("a named food is not eaten again while its dose is in force", "[landing]")
{
    // Gourmet's goat cheese: a lasting boon and an instant hunger effect.
    Snapshot s = Healthy();
    s.potions.Add(kGoatCheese, 3, ConsumableKind::Food, Boon("Fortify Magicka Regeneration", 25.0f, 1200.0f));
    s.potions.carried.back().effects.push_back({"Restore Hunger Medium", 0.0f, 0.0f});
    const Rule eat = Do(Named(ActionKind::EatFood, kGoatCheese));

    REQUIRE(VerdictOf(eat, s) == Verdict::Fired);
    // The same dose in force, by its record.
    s.potions.running = {{"Fortify Magicka Regeneration", 25.0f}};
    REQUIRE(VerdictOf(eat, s) == Verdict::EffectActive);
    // A stronger one of the name in force: nothing to add.
    s.potions.running = {{"Fortify Magicka Regeneration", 40.0f}};
    REQUIRE(VerdictOf(eat, s) == Verdict::EffectActive);
    // A weaker one in force: the cheese is an upgrade.
    s.potions.running = {{"Fortify Magicka Regeneration", 10.0f}};
    REQUIRE(VerdictOf(eat, s) == Verdict::Fired);
}

TEST_CASE("a thing with two boons is taken while either would land", "[landing]")
{
    Snapshot s = Healthy();
    s.potions.Add(kTwoBoons, 2, ConsumableKind::Potion, Boon("Fortify Destruction", 25.0f));
    s.potions.carried.back().effects.push_back(Boon("Regenerate Magicka", 50.0f));
    const Rule drink = Do(Named(ActionKind::DrinkPotion, kTwoBoons));

    s.potions.running = {{"Fortify Destruction", 25.0f}};
    REQUIRE(VerdictOf(drink, s) == Verdict::Fired);
    s.potions.running = {{"Fortify Destruction", 25.0f}, {"Regenerate Magicka", 50.0f}};
    REQUIRE(VerdictOf(drink, s) == Verdict::EffectActive);
}

TEST_CASE("an instant thing is not judged by what is in force, and a bane counts for nothing", "[landing]")
{
    // A restore: nothing lasts, so nothing in force covers it; its cooldown
    // spaces it, as before.
    Snapshot s = Healthy();
    s.potions.running = {{"Restore Health", 50.0f}};
    REQUIRE(VerdictOf(Do(Named(ActionKind::DrinkPotion, kHealthPotion)), s) == Verdict::Fired);

    // A wine: its resist lands or not, its regeneration damage is not what
    // it is drunk for.
    s.potions.Add(kWine, 2, ConsumableKind::Potion, Boon("Resist Frost", 10.0f));
    s.potions.carried.back().effects.push_back(Bane("Damage Stamina Regeneration", 50.0f));
    const Rule drink = Do(Named(ActionKind::DrinkPotion, kWine));
    s.potions.running = {{"Resist Frost", 10.0f}};
    REQUIRE(VerdictOf(drink, s) == Verdict::EffectActive);
    s.potions.running.clear();
    REQUIRE(VerdictOf(drink, s) == Verdict::Fired);
}

TEST_CASE("a cast on oneself is not repeated while what it puts up is in force", "[landing]")
{
    Snapshot s = Healthy();
    s.spells.known.push_back(kOakflesh);
    s.spells.lasting.push_back({kOakflesh, {{"Oakflesh", 40.0f}}});
    const Rule cast = Do(Named(ActionKind::CastSpell, kOakflesh));

    REQUIRE(VerdictOf(cast, s) == Verdict::Fired);
    // A scroll's Oakflesh running: the spell would add nothing.
    s.spells.running = {{"Oakflesh", 40.0f}};
    REQUIRE(VerdictOf(cast, s) == Verdict::EffectActive);
    // A weaker armour of the name: the spell is an upgrade.
    s.spells.running = {{"Oakflesh", 20.0f}};
    REQUIRE(VerdictOf(cast, s) == Verdict::Fired);

    // Its own effects running still say so, whatever the lasting list.
    s.spells.running.clear();
    s.spells.active.push_back(kOakflesh);
    REQUIRE(VerdictOf(cast, s) == Verdict::EffectActive);
}

TEST_CASE("alchemy and spells are each asked of their own", "[landing]")
{
    // A potion's Oakflesh-named effect does not cover the spell, and the
    // spell's does not cover the potion: each family stacks with the other.
    Snapshot s = Healthy();
    s.spells.known.push_back(kOakflesh);
    s.spells.lasting.push_back({kOakflesh, {{"Oakflesh", 40.0f}}});
    s.potions.running = {{"Oakflesh", 40.0f}};
    REQUIRE(VerdictOf(Do(Named(ActionKind::CastSpell, kOakflesh)), s) == Verdict::Fired);

    s.potions.running.clear();
    s.potions.Add(kGoatCheese, 3, ConsumableKind::Food, Boon("Fortify Magicka Regeneration", 25.0f, 1200.0f));
    s.spells.running = {{"Fortify Magicka Regeneration", 25.0f}};
    REQUIRE(VerdictOf(Do(Named(ActionKind::EatFood, kGoatCheese)), s) == Verdict::Fired);
}

TEST_CASE("a cast on another is judged only by its own effects on the caster", "[landing]")
{
    // The snapshot holds no one else's effects in force, so the caster's
    // cannot stand in for them.
    Snapshot s = Healthy();
    Player(s);
    s.spells.known.push_back(kOakflesh);
    s.spells.lasting.push_back({kOakflesh, {{"Oakflesh", 40.0f}}});
    s.spells.running = {{"Oakflesh", 40.0f}};
    REQUIRE(VerdictOf(Do(Named(ActionKind::CastSpell, kOakflesh), ActionTargetKind::Player), s) == Verdict::Fired);
}

TEST_CASE("a policy is still judged by the one effect it names", "[landing]")
{
    // A food with two boons, chosen for the health one: the stamina one not
    // running is not a reason to eat it again.
    Snapshot s = Healthy();
    s.potions.Add(kGoatCheese, 3, ConsumableKind::Food, Boon("Fortify Health Regeneration", 25.0f, 600.0f));
    s.potions.carried.back().effects.push_back(Boon("Fortify Stamina Regeneration", 25.0f, 600.0f));
    Action strongest;
    strongest.kind = ActionKind::EatStrongestFood;
    strongest.effect = "Fortify Health Regeneration";
    const Rule eat = Do(strongest);

    REQUIRE(VerdictOf(eat, s) == Verdict::Fired);
    s.potions.running = {{"Fortify Health Regeneration", 25.0f}};
    REQUIRE(VerdictOf(eat, s) == Verdict::EffectActive);
}
