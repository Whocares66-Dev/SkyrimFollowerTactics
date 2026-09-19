// The tick's choice for one actor: which list, and the fight's edges. No
// Skyrim; the game side reads the actor and hands the facts in.

#include <catch2/catch_test_macros.hpp>

#include "Build.h"
#include "core/Tick.h"

using namespace ft;

namespace
{

// A list of two actions, part way through: the first done, the second
// still to run.
EvalContext::Sequence InProgress()
{
    Rule two = test::HealBelow(0.5f);
    two.actions.push_back(two.actions.front());
    return {0, two, 0, 0, 1};
}

ActorTick::Now Quiet()
{
    return {};
}

ActorTick::Now Fighting()
{
    ActorTick::Now now;
    now.fighting = true;
    return now;
}

} // namespace

TEST_CASE("out of a fight with no idle rules there is nothing to decide", "[tick]")
{
    ActorTick tick;
    EvalContext ctx;
    REQUIRE_FALSE(tick.Plan(ctx, Quiet()));
    REQUIRE_FALSE(tick.fighting);

    // With idle rules, the idle list, and no edge.
    ActorTick::Now idle = Quiet();
    idle.idleHasRules = true;
    const TickPlan plan = tick.Plan(ctx, idle);
    REQUIRE(plan.list == Moment::Idle);
    REQUIRE_FALSE(plan.began);
    REQUIRE_FALSE(plan.ended);
    REQUIRE(tick.moment == Moment::Idle);
}

TEST_CASE("a fight is the combat list's: its first tick begins it, one tick after it ends it", "[tick]")
{
    ActorTick tick;
    EvalContext ctx;

    TickPlan plan = tick.Plan(ctx, Fighting());
    REQUIRE(plan.list == Moment::Combat);
    REQUIRE(plan.began);
    REQUIRE_FALSE(plan.ended);
    REQUIRE(tick.fighting);

    plan = tick.Plan(ctx, Fighting());
    REQUIRE(plan.list == Moment::Combat);
    REQUIRE_FALSE(plan.began);

    // The farewell evaluation: the combat list once more, out of the fight.
    plan = tick.Plan(ctx, Quiet());
    REQUIRE(plan.list == Moment::Combat);
    REQUIRE(plan.ended);
    REQUIRE_FALSE(plan.began);
    REQUIRE_FALSE(tick.fighting);

    // Then nothing: no idle rules.
    REQUIRE_FALSE(tick.Plan(ctx, Quiet()));
}

TEST_CASE("held through an edge, the actor still owes it", "[tick]")
{
    ActorTick tick;
    EvalContext ctx;

    // Bleeding out as the fight begins: nothing runs, and the standing
    // does not move.
    ActorTick::Now down = Fighting();
    down.held = true;
    REQUIRE_FALSE(tick.Plan(ctx, down));
    REQUIRE_FALSE(tick.fighting);

    // Up again: the fight's first evaluation, late.
    TickPlan plan = tick.Plan(ctx, Fighting());
    REQUIRE(plan.list == Moment::Combat);
    REQUIRE(plan.began);

    // Down as it ends: the farewell waits too, and the idle list does not
    // jump the queue.
    ActorTick::Now downAfter = Quiet();
    downAfter.held = true;
    downAfter.idleHasRules = true;
    REQUIRE_FALSE(tick.Plan(ctx, downAfter));
    REQUIRE(tick.fighting);

    ActorTick::Now after = Quiet();
    after.idleHasRules = true;
    plan = tick.Plan(ctx, after);
    REQUIRE(plan.list == Moment::Combat);
    REQUIRE(plan.ended);
    plan = tick.Plan(ctx, after);
    REQUIRE(plan.list == Moment::Idle);
}

TEST_CASE("a Combat end list in progress runs on out of the fight, then the idle list", "[tick]")
{
    ActorTick tick;
    EvalContext ctx;
    REQUIRE(tick.Plan(ctx, Fighting()).began);
    TickPlan plan = tick.Plan(ctx, Quiet());
    REQUIRE(plan.ended);

    // The farewell evaluation queued a list; it is the combat list's.
    ctx.pending = InProgress();
    ActorTick::Now after = Quiet();
    after.idleHasRules = true;
    plan = tick.Plan(ctx, after);
    REQUIRE(plan.list == Moment::Combat);
    REQUIRE_FALSE(plan.ended);
    REQUIRE_FALSE(plan.began);

    // Through: the idle list has its turn.
    ctx.pending = {};
    plan = tick.Plan(ctx, after);
    REQUIRE(plan.list == Moment::Idle);
}

TEST_CASE("an idle list in progress is evaluated with no idle rules left, and a fight interrupts it", "[tick]")
{
    ActorTick tick;
    EvalContext ctx;
    ActorTick::Now idle = Quiet();
    idle.idleHasRules = true;
    REQUIRE(tick.Plan(ctx, idle).list == Moment::Idle);
    ctx.pending = InProgress();

    // The rules were deleted under it: the list in progress still runs.
    TickPlan plan = tick.Plan(ctx, Quiet());
    REQUIRE(plan.list == Moment::Idle);

    // A fight begins: the combat list, on its edge, whatever the idle list
    // was doing, and the idle sequence is dropped on the handoff (the
    // evaluator would drop it on the edge too).
    plan = tick.Plan(ctx, Fighting());
    REQUIRE(plan.list == Moment::Combat);
    REQUIRE(plan.began);
    REQUIRE_FALSE(ctx.InProgress());
}

TEST_CASE("a list switched off is silenced: not owed an edge, and its own sequence dropped", "[tick]")
{
    ActorTick tick;
    EvalContext ctx;
    ActorTick::Now off = Fighting();
    off.combatEnabled = false;

    // The fight runs with the combat list off: nothing is evaluated, and
    // the fight is not owed its first evaluation when the list comes back.
    REQUIRE_FALSE(tick.Plan(ctx, off));
    REQUIRE(tick.fighting);
    TickPlan plan = tick.Plan(ctx, Fighting());
    REQUIRE(plan.list == Moment::Combat);
    REQUIRE_FALSE(plan.began);

    // Off as the fight ends, with a Combat end list waiting: neither the
    // farewell nor the list holds the idle list up. It runs this tick.
    ctx.pending = InProgress();
    ActorTick::Now offAfter = Quiet();
    offAfter.combatEnabled = false;
    offAfter.idleHasRules = true;
    plan = tick.Plan(ctx, offAfter);
    REQUIRE(plan.list == Moment::Idle);
    REQUIRE_FALSE(plan.ended);
    REQUIRE_FALSE(ctx.InProgress());
    REQUIRE_FALSE(tick.fighting);

    // The idle list off: nothing out of a fight, and a fight is still the
    // combat list's.
    ActorTick::Now idleOff = Quiet();
    idleOff.idleEnabled = false;
    idleOff.idleHasRules = true;
    REQUIRE_FALSE(tick.Plan(ctx, idleOff));
    idleOff.fighting = true;
    plan = tick.Plan(ctx, idleOff);
    REQUIRE(plan.list == Moment::Combat);
    REQUIRE(plan.began);
}

TEST_CASE("a switched-off list drops only its own sequence", "[tick]")
{
    // The idle list is part way through when the fight begins with the
    // combat list off: the idle sequence is not the combat list's to drop.
    // (The evaluator would drop it on the fight's edge, had the combat list
    // been evaluated; the next idle evaluation, after the fight, finds it
    // where it was.)
    ActorTick tick;
    EvalContext ctx;
    ActorTick::Now idle = Quiet();
    idle.idleHasRules = true;
    REQUIRE(tick.Plan(ctx, idle).list == Moment::Idle);
    ctx.pending = InProgress();

    ActorTick::Now off = Fighting();
    off.combatEnabled = false;
    REQUIRE_FALSE(tick.Plan(ctx, off));
    REQUIRE(ctx.InProgress());

    // The combat list switched on mid-fight: its edge is spent, so the
    // evaluator will not drop the idle sequence itself, and it must not
    // run as combat tactics (review, 2026-09-19). The handoff drops it,
    // and an empty combat list fires nothing.
    TickPlan plan = tick.Plan(ctx, Fighting());
    REQUIRE(plan.list == Moment::Combat);
    REQUIRE_FALSE(plan.began);
    REQUIRE_FALSE(ctx.InProgress());
    Snapshot s = test::Healthy();
    s.health = {40.0f, 100.0f};
    s.combatBegan = plan.began;
    REQUIRE_FALSE(Evaluate(RuleSet{}, s, ctx).Fired());

    // With a combat rule of its own, that rule gets the tick, not the
    // idle list's leftover.
    ActorTick again;
    EvalContext ctx2;
    REQUIRE(again.Plan(ctx2, idle).list == Moment::Idle);
    ctx2.pending = InProgress();
    REQUIRE_FALSE(again.Plan(ctx2, off));
    plan = again.Plan(ctx2, Fighting());
    REQUIRE(plan.list == Moment::Combat);
    RuleSet combat;
    combat.rules.push_back(test::HealBelow(0.5f, "combat heal"));
    const Decision d = Evaluate(combat, s, ctx2);
    REQUIRE(d.Fired());
    REQUIRE(d.rule.label == "combat heal");
}

TEST_CASE("the spells the rules name, from both lists, each once", "[tick]")
{
    const auto cast = [](std::uint32_t form) {
        Rule r = test::HealBelow(0.5f);
        r.FirstAction().kind = ActionKind::CastSpell;
        r.FirstAction().form = form;
        return r;
    };
    ActorRules rules;
    REQUIRE(SpellsNamedBy(rules).empty());
    rules.combat.rules.push_back(cast(0x12FCD));
    rules.combat.rules.push_back(test::HealBelow(0.5f)); // a drink names no spell
    rules.combat.rules.push_back(cast(0));               // a cast of nothing yet
    rules.idle.rules.push_back(cast(0x2F3B8));
    rules.idle.rules.push_back(cast(0x12FCD)); // the same spell in both lists
    const auto named = SpellsNamedBy(rules);
    REQUIRE(named == std::vector<std::uint32_t>{0x12FCD, 0x2F3B8});
    REQUIRE(&rules.Of(Moment::Idle) == &rules.idle);
    REQUIRE(&rules.Of(Moment::Combat) == &rules.combat);
}
