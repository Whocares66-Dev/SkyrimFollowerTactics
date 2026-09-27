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
constexpr std::uint32_t kOakfleshEffect = 0x0005AD5D;
constexpr std::uint32_t kOakfleshScroll = 0x000A4495;
constexpr std::uint32_t kCourage = 0x0004DEE8;
constexpr std::uint32_t kCourageEffect = 0x0001EA79;
constexpr std::uint32_t kRally = 0x0004DEEC;
constexpr std::uint32_t kRallyEffect = 0x0001EA76;

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

TEST_CASE("a named thing nothing of which would take on them is not taken", "[landing]")
{
    // A vampire's blood potion, a player-only food: the engine's landing
    // test refuses every effect of it on the follower (PotionStock's
    // Carried::refused), so it would do nothing.
    Snapshot s = Healthy();
    s.potions.Add(kTwoBoons, 2, ConsumableKind::Potion, Boon("Fortify Health", 50.0f));
    const Rule drink = Do(Named(ActionKind::DrinkPotion, kTwoBoons));
    REQUIRE(VerdictOf(drink, s) == Verdict::Fired);

    s.potions.carried.back().effects.clear();
    s.potions.carried.back().refused = true;
    REQUIRE(VerdictOf(drink, s) == Verdict::NoEffect);

    // And a policy never chooses it: nothing of it is there to choose by.
    Action strongest;
    strongest.kind = ActionKind::DrinkStrongest;
    strongest.effect = "Fortify Health";
    REQUIRE(VerdictOf(Do(strongest), s) == Verdict::NoResource);
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
    Lands(s, kOakflesh, s.self, {{"Oakflesh", 40.0f, kOakflesh, kOakfleshEffect}});
    const Rule cast = Do(Named(ActionKind::CastSpell, kOakflesh));

    REQUIRE(VerdictOf(cast, s) == Verdict::Fired);
    // A scroll's Oakflesh running: the spell would add nothing.
    s.traits.running = {{"Oakflesh", 40.0f, kOakfleshScroll, kOakfleshEffect}};
    REQUIRE(VerdictOf(cast, s) == Verdict::EffectActive);
    // A weaker armour of the name: the spell is an upgrade.
    s.traits.running = {{"Oakflesh", 20.0f}};
    REQUIRE(VerdictOf(cast, s) == Verdict::Fired);
}

TEST_CASE("a hidden effect is covered by its own record only", "[landing]")
{
    // A power whose one lasting effect is hidden: no name to go by, so only
    // the same effect of the same power in force covers it -- a refresh.
    constexpr std::uint32_t kPower = 0x000E40C4;
    constexpr std::uint32_t kHidden = 0x000E40C5;
    Snapshot s = Healthy();
    s.spells.known.push_back(kPower);
    Lands(s, kPower, s.self, {{"", 0.0f, kPower, kHidden}});
    const Rule use = Do(Named(ActionKind::UsePower, kPower));

    REQUIRE(VerdictOf(use, s) == Verdict::Fired);
    s.traits.running = {{"", 0.0f, 0x00012345, kHidden}}; // the effect, from another power
    REQUIRE(VerdictOf(use, s) == Verdict::Fired);
    s.traits.running = {{"", 0.0f, kPower, kHidden}};
    REQUIRE(VerdictOf(use, s) == Verdict::EffectActive);
}

TEST_CASE("alchemy and spells are each asked of their own", "[landing]")
{
    // A potion's Oakflesh-named effect does not cover the spell, and the
    // spell's does not cover the potion: each family stacks with the other.
    Snapshot s = Healthy();
    s.spells.known.push_back(kOakflesh);
    Lands(s, kOakflesh, s.self, {{"Oakflesh", 40.0f, kOakflesh, kOakfleshEffect}});
    s.potions.running = {{"Oakflesh", 40.0f}};
    REQUIRE(VerdictOf(Do(Named(ActionKind::CastSpell, kOakflesh)), s) == Verdict::Fired);

    s.potions.running.clear();
    s.potions.Add(kGoatCheese, 3, ConsumableKind::Food, Boon("Fortify Magicka Regeneration", 25.0f, 1200.0f));
    s.traits.running = {{"Fortify Magicka Regeneration", 25.0f}};
    REQUIRE(VerdictOf(Do(Named(ActionKind::EatFood, kGoatCheese)), s) == Verdict::Fired);
}

TEST_CASE("a cast on another is judged by what is in force on them, not on the caster", "[landing]")
{
    Snapshot s = Healthy();
    ActorView &player = Player(s);
    s.spells.known.push_back(kCourage);
    Lands(s, kCourage, kPlayerFormID, {{"Courage", 100.0f, kCourage, kCourageEffect}}, SpellState::Reach::Target);
    const Rule cast = Do(Named(ActionKind::CastSpell, kCourage), ActionTargetKind::Player);

    // The caster's own Courage is no reason to keep it from the player.
    s.traits.running = {{"Courage", 100.0f, kCourage, kCourageEffect}};
    REQUIRE(VerdictOf(cast, s) == Verdict::Fired);
    // The player's is.
    player.traits.running = {{"Courage", 100.0f, kCourage, kCourageEffect}};
    REQUIRE(VerdictOf(cast, s) == Verdict::EffectActive);
}

TEST_CASE("a cast nothing of which would take is not made, and one the engine judges no further is", "[landing]")
{
    Snapshot s = Healthy();
    ActorView &player = Player(s);
    s.spells.known.push_back(kCourage);
    // Every effect refused there -- an undead ally without Master of the
    // Mind, an enemy above a Calm's level: nothing would take.
    SpellState::Landing &landing = Lands(s, kCourage, kPlayerFormID, {}, SpellState::Reach::Target);
    const Rule cast = Do(Named(ActionKind::CastSpell, kCourage), ActionTargetKind::Player);
    REQUIRE(VerdictOf(cast, s) == Verdict::NoEffect);

    // An instant that would take -- a heal, a script's moment -- is always
    // worth it: nothing in force answers for it.
    landing.acts = true;
    player.traits.running = {{"Courage", 100.0f, kCourage, kCourageEffect}};
    REQUIRE(VerdictOf(cast, s) == Verdict::Fired);

    // Someone the game side did not judge, and a spell it could not read:
    // it would.
    s.spells.landings.clear();
    REQUIRE(VerdictOf(cast, s) == Verdict::Fired);
    s.spells.casts.clear();
    REQUIRE(VerdictOf(cast, s) == Verdict::Fired);
}

TEST_CASE("an area cast is made while anyone it reaches would gain", "[landing]")
{
    constexpr ActorId kLydia = 0x202;
    Snapshot s = Healthy();
    s.allies.push_back({kLydia, {100.0f, 100.0f}, 300.0f});
    s.spells.known.push_back(kRally);
    const RunningEffect rally{"Rally", 100.0f, kRally, kRallyEffect};
    Lands(s, kRally, s.self, {rally}, SpellState::Reach::Area);
    Lands(s, kRally, kLydia, {rally});
    const Rule cast = Do(Named(ActionKind::CastSpell, kRally));

    s.traits.running = {rally};
    REQUIRE(VerdictOf(cast, s) == Verdict::Fired); // Lydia would gain
    s.allies[0].traits.running = {rally};
    REQUIRE(VerdictOf(cast, s) == Verdict::EffectActive);
}

TEST_CASE("a dual cast is weighed as one", "[landing]")
{
    Snapshot s = Healthy();
    s.spells.known.push_back(kOakflesh);
    s.spells.costs.push_back({kOakflesh, 10.0f, true, 28.0f});
    Lands(s, kOakflesh, s.self, {{"Oakflesh", 40.0f, kOakflesh, kOakfleshEffect, true}}, SpellState::Reach::Self, true);
    Action dual = Named(ActionKind::CastSpell, kOakflesh);
    dual.dual = true;

    // A single cast's in force: the dual cast's is stronger.
    s.traits.running = {{"Oakflesh", 40.0f, kOakflesh, kOakfleshEffect}};
    REQUIRE(VerdictOf(Do(dual), s) == Verdict::Fired);
    s.traits.running.back().dual = true;
    REQUIRE(VerdictOf(Do(dual), s) == Verdict::EffectActive);
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
