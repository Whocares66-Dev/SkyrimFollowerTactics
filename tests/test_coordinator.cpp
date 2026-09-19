// The tick as the game runs it for one actor, over the production pieces
// together: the planner (ActorTick), the evaluator, the action's result
// as the game reports it, and the cooldown restarted when a requested
// action is over (RestartCooldown). The game's Tick does exactly this per
// actor (game/Tactics.cpp); this harness does it with the action's result
// scripted, so the composition is tested rather than the stages alone
// (the review of 2026-09-19 found a defect between two tested stages).

#include <catch2/catch_test_macros.hpp>

#include "Build.h"
#include "core/Evaluator.h"
#include "core/Tick.h"

#include <optional>
#include <string>
#include <vector>

using namespace ft;

namespace
{

constexpr std::uint32_t kHeal = 0x0002F3B8;

Rule CastHeal()
{
    Rule cast;
    cast.subject = SubjectKind::Self;
    cast.predicate = PredicateKind::Any;
    cast.actionTarget = ActionTargetKind::Self;
    cast.FirstAction().kind = ActionKind::CastSpell;
    cast.FirstAction().form = kHeal;
    cast.label = "cast heal";
    return cast;
}

// One actor under the tick, as game/Tactics.cpp keeps them: the planner's
// standing, the evaluator's context, the requested action in flight.
struct Actor
{
    ActorTick tick;
    EvalContext ctx;
    std::optional<Decision::Step> inFlight;
    std::vector<std::string> fired; // the labels, in order
};

// What the game's Execute would answer.
enum class Result
{
    Performed,
    Requested,
    Failed
};

// One tick: the completion of a request that is over, the plan, the
// evaluation, the dispatch. `busy` is what the game reads of the actor
// (IsMidCast, IsMidBash); `execute` is the game's dispatch.
template <class Execute>
std::optional<Decision> Tick(Actor &actor, const ActorRules &rules, Snapshot snapshot, bool fighting, bool busy,
                             double now, Execute execute)
{
    if (actor.inFlight && !busy)
    {
        RestartCooldown(actor.ctx, actor.inFlight->action, actor.inFlight->target, now);
        actor.inFlight.reset();
    }
    // As RuntimeCapabilities reads it: ours in flight holds every action.
    actor.ctx.caps.busy.fill(busy);
    ActorTick::Now seen;
    seen.fighting = fighting;
    seen.idleHasRules = !rules.idle.rules.empty();
    const TickPlan plan = actor.tick.Plan(actor.ctx, seen);
    if (!plan)
        return std::nullopt;
    snapshot.now = now;
    snapshot.inCombat = fighting;
    snapshot.combatBegan = plan.began;
    snapshot.combatEnded = plan.ended;
    const Decision d = Evaluate(rules.Of(*plan.list), snapshot, actor.ctx);
    if (d.Fired())
    {
        actor.fired.push_back(d.rule.label);
        if (execute(*d.step) == Result::Requested)
            actor.inFlight = *d.step;
    }
    return d;
}

} // namespace

TEST_CASE("a requested cast's cooldown runs from its end, not from the decision", "[coordinator]")
{
    ActorRules rules;
    rules.combat.rules.push_back(CastHeal());
    Snapshot s = test::Healthy();
    s.spells.known.push_back(kHeal);
    Actor lydia;
    const auto requested = [](const Decision::Step &) { return Result::Requested; };

    // The fight's first tick: the cast fires and is in flight.
    auto d = Tick(lydia, rules, s, true, false, 100.0, requested);
    REQUIRE(d);
    REQUIRE(d->Fired());
    REQUIRE(lydia.inFlight);
    REQUIRE(lydia.inFlight->action.form == kHeal);

    // Mid-cast: the rule is on the decision's own cooldown, and past that
    // the capabilities say busy -- ours in flight holds every action -- so
    // nothing fires again however long the cast takes.
    const double minimum = MinimumCooldown(ActionKind::CastSpell);
    for (double now = 100.5; now <= 100.0 + minimum + 0.5; now += 0.5)
    {
        d = Tick(lydia, rules, s, true, true, now, requested);
        REQUIRE_FALSE(d->Fired());
    }
    REQUIRE(lydia.inFlight);
    REQUIRE(lydia.fired.size() == 1);

    // Over at 102.5: the cooldown starts again from here, replacing the
    // decision's, so the next cast waits the minimum from the end.
    const double over = 102.5;
    d = Tick(lydia, rules, s, true, false, over, requested);
    REQUIRE_FALSE(lydia.inFlight);
    REQUIRE_FALSE(d->Fired());
    const double again = over + minimum;
    d = Tick(lydia, rules, s, true, false, again - 0.1, requested);
    REQUIRE_FALSE(d->Fired());
    d = Tick(lydia, rules, s, true, false, again, requested);
    REQUIRE(d->Fired());
    REQUIRE(lydia.fired.size() == 2);
}

TEST_CASE("a performed action restarts nothing; a failed one is not in flight", "[coordinator]")
{
    ActorRules rules;
    rules.combat.rules.push_back(test::HealBelow(0.5f, "heal"));
    Snapshot s = test::Healthy();
    s.health = {40.0f, 100.0f};
    Actor lydia;

    auto d = Tick(lydia, rules, s, true, false, 100.0, [](const Decision::Step &) { return Result::Performed; });
    REQUIRE(d->Fired());
    REQUIRE_FALSE(lydia.inFlight);
    // The potion's cooldown is the decision's, and runs from 100.
    const double again = 100.0 + MinimumCooldown(ActionKind::DrinkStrongest);
    REQUIRE_FALSE(Tick(lydia, rules, s, true, false, again - 0.1, [](const Decision::Step &) {
                      return Result::Performed;
                  })->Fired());
    REQUIRE(
        Tick(lydia, rules, s, true, false, again, [](const Decision::Step &) { return Result::Performed; })->Fired());

    Actor failing;
    d = Tick(failing, rules, s, true, false, 100.0, [](const Decision::Step &) { return Result::Failed; });
    REQUIRE(d->Fired());
    REQUIRE_FALSE(failing.inFlight);
}

TEST_CASE("two actors under one tick share nothing", "[coordinator]")
{
    ActorRules rules;
    rules.combat.rules.push_back(CastHeal());
    Snapshot s = test::Healthy();
    s.spells.known.push_back(kHeal);
    Actor lydia;
    Actor jenassa;
    const auto requested = [](const Decision::Step &) { return Result::Requested; };

    REQUIRE(Tick(lydia, rules, s, true, false, 100.0, requested)->Fired());
    // Jenassa joins the fight a second later: a first evaluation, a cast
    // and a cooldown of Jenassa's own, while Lydia is mid-cast.
    auto d = Tick(jenassa, rules, s, true, false, 101.0, requested);
    REQUIRE(d->Fired());
    REQUIRE(jenassa.inFlight);
    REQUIRE(lydia.inFlight);
    REQUIRE_FALSE(Tick(lydia, rules, s, true, true, 101.0, requested)->Fired());
}

TEST_CASE("the fight's edges through the whole tick: a Combat begins rule fires once, after a hold too",
          "[coordinator]")
{
    Rule onBegin = test::HealBelow(2.0f, "on begin");
    onBegin.predicate = PredicateKind::CombatBegins;
    ActorRules rules;
    rules.combat.rules.push_back(onBegin);
    Snapshot s = test::Healthy();
    Actor lydia;
    const auto performed = [](const Decision::Step &) { return Result::Performed; };

    REQUIRE(Tick(lydia, rules, s, true, false, 100.0, performed)->Fired());
    REQUIRE_FALSE(Tick(lydia, rules, s, true, false, 100.5, performed)->Fired());
    // The fight ends and another begins: once more.
    REQUIRE_FALSE(Tick(lydia, rules, s, false, false, 101.0, performed)->Fired());
    REQUIRE_FALSE(Tick(lydia, rules, s, false, false, 101.5, performed));
    REQUIRE(Tick(lydia, rules, s, true, false, 102.0, performed)->Fired());
    REQUIRE(lydia.fired.size() == 2);

    // A new session: the standing is fresh, and the next fight begins anew.
    lydia = Actor{};
    REQUIRE(Tick(lydia, rules, s, true, false, 200.0, performed)->Fired());
}
