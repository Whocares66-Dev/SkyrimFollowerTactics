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

TEST_CASE("a cast on oneself is not repeated while anything from it is in force", "[landing]")
{
    // The combat AI's own test (45344): an effect running whose spell is
    // this one, whatever its strength.
    Snapshot s = Healthy();
    s.spells.known.push_back(kOakflesh);
    Lands(s, kOakflesh, s.self, true);
    const Rule cast = Do(Named(ActionKind::CastSpell, kOakflesh));

    REQUIRE(VerdictOf(cast, s) == Verdict::Fired);
    s.traits.running = {{"Oakflesh", 40.0f, kOakflesh, kOakfleshEffect}};
    REQUIRE(VerdictOf(cast, s) == Verdict::EffectActive);
    s.traits.running = {{"Oakflesh", 20.0f, kOakflesh, kOakfleshEffect}};
    REQUIRE(VerdictOf(cast, s) == Verdict::EffectActive);
    // Another spell's Oakflesh is not this one's, as the engine's AI has it:
    // a scroll's does not keep the spell back.
    s.traits.running = {{"Oakflesh", 40.0f, kOakfleshScroll, kOakfleshEffect}};
    REQUIRE(VerdictOf(cast, s) == Verdict::Fired);
}

TEST_CASE("what runs is asked for the spell it came from, hidden or not", "[landing]")
{
    // A power whose one lasting effect is hidden: the same effect from
    // another power is not this one.
    constexpr std::uint32_t kPower = 0x000E40C4;
    constexpr std::uint32_t kHidden = 0x000E40C5;
    Snapshot s = Healthy();
    s.spells.known.push_back(kPower);
    Lands(s, kPower, s.self, true);
    const Rule use = Do(Named(ActionKind::UsePower, kPower));

    REQUIRE(VerdictOf(use, s) == Verdict::Fired);
    s.traits.running = {{"", 0.0f, 0x00012345, kHidden}};
    REQUIRE(VerdictOf(use, s) == Verdict::Fired);
    s.traits.running = {{"", 0.0f, kPower, kHidden}};
    REQUIRE(VerdictOf(use, s) == Verdict::EffectActive);
}

TEST_CASE("a shout is in effect while anything from any of its words runs", "[landing]")
{
    // Dragon Aspect: the aspect running from word one, the rule shouting
    // word three. Stormcrown's hidden Dispel on each word, which lands and
    // runs no time, no longer has a say (2026-09-26).
    constexpr std::uint32_t kAspect = 0x0201DF92;
    constexpr std::uint32_t kWordOne = 0x0201DF91;
    constexpr std::uint32_t kWordThree = 0x0201DF99;
    Snapshot s = Healthy();
    s.spells.known.push_back(kAspect);
    Lands(s, kAspect, s.self, true);
    s.spells.casts.back().from = {kWordOne, 0x0201DF96, kWordThree};
    const Rule shout = Do(Named(ActionKind::Shout, kAspect));

    REQUIRE(VerdictOf(shout, s) == Verdict::Fired);
    s.traits.running = {{"Dragon Aspect", 100.0f, kWordOne, 0x02021731}};
    REQUIRE(VerdictOf(shout, s) == Verdict::EffectActive);
}

TEST_CASE("a stream is never held back by its own effect running", "[landing]")
{
    // The AI's gates skip a concentration spell: its effect runs only while
    // it is being cast.
    constexpr std::uint32_t kHealing = 0x00012FCC;
    Snapshot s = Healthy();
    s.spells.known.push_back(kHealing);
    Lands(s, kHealing, s.self, true);
    s.spells.casts.back().concentration = true;
    s.traits.running = {{"Restore Health", 10.0f, kHealing, 0x0004D3F0}};
    REQUIRE(VerdictOf(Do(Named(ActionKind::CastSpell, kHealing)), s) == Verdict::Fired);
}

TEST_CASE("alchemy and spells are each asked of their own", "[landing]")
{
    // A potion's Oakflesh-named effect does not cover the spell, and the
    // spell's does not cover the potion: each family stacks with the other.
    Snapshot s = Healthy();
    s.spells.known.push_back(kOakflesh);
    Lands(s, kOakflesh, s.self, true);
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
    Lands(s, kCourage, kPlayerFormID, true, SpellState::Reach::Target);
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
    SpellState::Landing &landing = Lands(s, kCourage, kPlayerFormID, false, SpellState::Reach::Target);
    const Rule cast = Do(Named(ActionKind::CastSpell, kCourage), ActionTargetKind::Player);
    REQUIRE(VerdictOf(cast, s) == Verdict::NoEffect);

    // Something of it would take, and nothing from it runs there: an
    // instant as much as a buff.
    landing.takes = true;
    REQUIRE(VerdictOf(cast, s) == Verdict::Fired);
    player.traits.running = {{"Courage", 100.0f, kCourage, kCourageEffect}};
    REQUIRE(VerdictOf(cast, s) == Verdict::EffectActive);

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
    const Rule cast = Do(Named(ActionKind::CastSpell, kRally));

    SECTION("on the caster as well")
    {
        Lands(s, kRally, s.self, true);
        LandsAbout(s, kRally, kLydia, true);
        s.traits.running = {rally};
        REQUIRE(VerdictOf(cast, s) == Verdict::Fired); // Lydia would gain
        s.allies[0].traits.running = {rally};
        REQUIRE(VerdictOf(cast, s) == Verdict::EffectActive);
    }

    SECTION("on those about the caster alone")
    {
        // Nothing of it takes on the caster: the allies are why it is cast.
        Lands(s, kRally, s.self, false);
        LandsAbout(s, kRally, kLydia, true);
        REQUIRE(VerdictOf(cast, s) == Verdict::Fired);
        s.allies[0].traits.running = {rally};
        REQUIRE(VerdictOf(cast, s) == Verdict::EffectActive);
    }
}

TEST_CASE("about the centre of a cast, only its area effects are weighed", "[landing]")
{
    // Dragonhide under Adamant: Fortify Armor on the caster, and Bastion,
    // with an area, on those about them when dual cast. An ally is judged by
    // Bastion alone: held to the caster's armour as well, a rule cast it over
    // and over with Bastion already on everyone (2026-09-26).
    constexpr ActorId kLydia = 0x202;
    constexpr std::uint32_t kDragonhide = 0x000CDB70;
    Snapshot s = Healthy();
    s.allies.push_back({kLydia, {100.0f, 100.0f}, 300.0f});
    s.spells.known.push_back(kDragonhide);
    s.spells.costs.push_back({kDragonhide, 50.0f, true, 60.0f});
    const RunningEffect armor{"Fortify Armor Rating", 200.0f, kDragonhide, 0x000CDB75, true};
    const RunningEffect bastion{"Armor - Bastion", 200.0f, kDragonhide, 0x0409862F, true};
    Lands(s, kDragonhide, s.self, true, SpellState::Reach::Self, true);
    // At the centre too, as it would be were the cast aimed at Lydia: not
    // consulted, since a spell on oneself is centred on the caster.
    Lands(s, kDragonhide, kLydia, true, SpellState::Reach::Self, true);
    SpellState::Landing &about = LandsAbout(s, kDragonhide, kLydia, true, true);
    Action dual = Named(ActionKind::CastSpell, kDragonhide);
    dual.dual = true;

    s.traits.running = {armor};
    REQUIRE(VerdictOf(Do(dual), s) == Verdict::Fired); // Lydia would gain Bastion
    s.allies[0].traits.running = {bastion};
    REQUIRE(VerdictOf(Do(dual), s) == Verdict::EffectActive);

    // Beyond Bastion's ring: nothing of it reaches Lydia, and nothing about
    // Lydia keeps the rule firing.
    s.allies[0].traits.running.clear();
    about.takes = false;
    REQUIRE(VerdictOf(Do(dual), s) == Verdict::EffectActive);
}

TEST_CASE("a dual cast is no upgrade over the spell's own single cast", "[landing]")
{
    // The engine's AI asks whether the spell is running, not how strongly:
    // a single cast's Oakflesh up keeps the dual cast back too.
    Snapshot s = Healthy();
    s.spells.known.push_back(kOakflesh);
    s.spells.costs.push_back({kOakflesh, 10.0f, true, 28.0f});
    Lands(s, kOakflesh, s.self, true, SpellState::Reach::Self, true);
    Action dual = Named(ActionKind::CastSpell, kOakflesh);
    dual.dual = true;

    REQUIRE(VerdictOf(Do(dual), s) == Verdict::Fired);
    s.traits.running = {{"Oakflesh", 40.0f, kOakflesh, kOakfleshEffect}};
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
