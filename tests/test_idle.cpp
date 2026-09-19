// The idle list: the second list, evaluated out of a fight as the combat
// list is in one (dev/PLAYER.md "Out of combat"). One context serves both:
// a cooldown is the action's whichever list spent it, and the fight's
// first edge drops whatever list was in progress. And what the idle list
// has no use for -- there is no enemy out of a fight -- is invalid in it.
//
// No Skyrim, no SKSE, no CommonLibSSE -- see dev/PLAN.md section 3.

#include <catch2/catch_test_macros.hpp>

#include "Build.h"
#include "core/Evaluator.h"

using namespace ft;
using ft::test::Healthy;

namespace
{

// IF self health below 200% (always) THEN self: drink the strongest of
// the effect.
Rule Always(const char *effect)
{
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::HealthPctBelow;
    r.conditionArg = 2.0f;
    r.actionTarget = ActionTargetKind::Self;
    r.FirstAction().kind = ActionKind::DrinkStrongest;
    r.FirstAction().effect = effect;
    return r;
}

RuleSet Idle(std::vector<Rule> rules)
{
    RuleSet rs;
    rs.moment = Moment::Idle;
    rs.rules = std::move(rules);
    return rs;
}

} // namespace

TEST_CASE("the idle list decides out of a fight, and nothing in one", "[idle]")
{
    const RuleSet idle = Idle({Always("Restore Health")});
    EvalContext ctx;
    Trace trace;

    Snapshot s = Healthy();
    s.inCombat = false;
    REQUIRE(Evaluate(idle, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::Fired);

    // In a fight the list is not looked at: every rule stays not reached,
    // and nothing is spent.
    s.inCombat = true;
    s.now += 5.0;
    const std::size_t spent = ctx.blocked.size();
    REQUIRE_FALSE(Evaluate(idle, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::NotReached);
    REQUIRE(ctx.blocked.size() == spent);
}

TEST_CASE("a fight's first edge drops an idle list in progress", "[idle]")
{
    Rule two = Always("Restore Health");
    two.actions.push_back(two.actions.front());
    two.actions.back().effect = "Restore Magicka";
    const RuleSet idle = Idle({two});
    RuleSet combat;
    combat.rules.push_back(Always("Restore Stamina"));
    EvalContext ctx;

    Snapshot s = Healthy();
    s.inCombat = false;
    REQUIRE(Evaluate(idle, s, ctx).Fired());
    REQUIRE(ctx.InProgress());

    // The fight begins: the combat list is evaluated on the edge, the idle
    // list's second action is not waited for, and the combat rule fires.
    s.inCombat = true;
    s.combatBegan = true;
    s.now += 0.5;
    const Decision d = Evaluate(combat, s, ctx);
    REQUIRE(d.Fired());
    REQUIRE(d.step->action.effect == "Restore Stamina");
    REQUIRE_FALSE(ctx.InProgress());
}

TEST_CASE("a cooldown spent in the fight holds in the idle list after it", "[idle]")
{
    RuleSet combat;
    combat.rules.push_back(Always("Restore Health"));
    const RuleSet idle = Idle({Always("Restore Health")});
    EvalContext ctx;
    Trace trace;

    Snapshot s = Healthy();
    REQUIRE(Evaluate(combat, s, ctx).Fired());

    // The fight ends on the same second: the idle list's same drink waits
    // for the first to show.
    s.inCombat = false;
    s.now += 0.5;
    REQUIRE_FALSE(Evaluate(idle, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::ActionCooldown);
    s.now += MinimumCooldown(ActionKind::DrinkStrongest);
    REQUIRE(Evaluate(idle, s, ctx, &trace).Fired());
}

TEST_CASE("what the idle list has no use for is invalid in it, and only in it", "[idle][validity]")
{
    for (const auto subject :
         {SubjectKind::Self, SubjectKind::Player, SubjectKind::Ally, SubjectKind::Follower, SubjectKind::Corpse})
        REQUIRE(IsSubjectValidIn(Moment::Idle, subject));
    REQUIRE_FALSE(IsSubjectValidIn(Moment::Idle, SubjectKind::Enemy));
    REQUIRE(IsSubjectValidIn(Moment::Combat, SubjectKind::Enemy));

    for (const auto predicate : {PredicateKind::CombatBegins, PredicateKind::CombatEnds, PredicateKind::HitBy,
                                 PredicateKind::Attacking, PredicateKind::AttackedBy})
    {
        REQUIRE_FALSE(IsPredicateValidIn(Moment::Idle, predicate));
        REQUIRE(IsPredicateValidIn(Moment::Combat, predicate));
    }
    // What one wields, the statuses, the measures, the summons and the
    // corpses are as fair a question out of a fight.
    for (const auto predicate : {PredicateKind::Any, PredicateKind::HitType, PredicateKind::Status,
                                 PredicateKind::HealthPctBelow, PredicateKind::ArmorLowest, PredicateKind::CorpseNone,
                                 PredicateKind::SummonActive, PredicateKind::WeaponPoisonNone})
        REQUIRE(IsPredicateValidIn(Moment::Idle, predicate));

    REQUIRE_FALSE(IsStatusValidIn(Moment::Idle, StatusKind::Fleeing));
    REQUIRE(IsStatusValidIn(Moment::Idle, StatusKind::Diseased));
    REQUIRE(IsStatusValidIn(Moment::Idle, StatusKind::BleedingOut));
    REQUIRE(IsStatusValidIn(Moment::Combat, StatusKind::Fleeing));

    REQUIRE_FALSE(IsActionTargetValidIn(Moment::Idle, ActionTargetKind::Enemy));
    REQUIRE_FALSE(IsActionTargetValidIn(Moment::Idle, ActionTargetKind::Attacker));
    REQUIRE(IsActionTargetValidIn(Moment::Idle, ActionTargetKind::Corpse));
    REQUIRE(IsActionTargetValidIn(Moment::Combat, ActionTargetKind::Enemy));

    for (const auto action : {ActionKind::Attack, ActionKind::PowerAttack, ActionKind::Bash, ActionKind::PowerBash})
    {
        REQUIRE_FALSE(IsActionValidIn(Moment::Idle, action));
        REQUIRE(IsActionValidIn(Moment::Combat, action));
    }
    REQUIRE(IsActionValidIn(Moment::Idle, ActionKind::CastSpell));
    REQUIRE(IsActionValidIn(Moment::Idle, ActionKind::EquipArmor));
}

TEST_CASE("an idle rule of what the list has no use for reports an invalid condition", "[idle]")
{
    Rule enemy = Always("Restore Health");
    enemy.subject = SubjectKind::Enemy;
    Rule edge = Always("Restore Health");
    edge.predicate = PredicateKind::CombatEnds;
    Rule fleeing = Always("Restore Health");
    fleeing.subject = SubjectKind::Player;
    fleeing.predicate = PredicateKind::Status;
    fleeing.statusKind = StatusKind::Fleeing;
    const RuleSet idle = Idle({enemy, edge, fleeing, Always("Restore Health")});
    EvalContext ctx;
    Trace trace;

    Snapshot s = Healthy();
    s.inCombat = false;
    const Decision d = Evaluate(idle, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 3);
    REQUIRE(trace.at(0) == Verdict::InvalidCondition);
    REQUIRE(trace.at(1) == Verdict::InvalidCondition);
    REQUIRE(trace.at(2) == Verdict::InvalidCondition);

    // The same three are sound in the combat list.
    RuleSet combat = idle;
    combat.moment = Moment::Combat;
    s.inCombat = true;
    EvalContext fresh;
    Evaluate(combat, s, fresh, &trace);
    REQUIRE(trace.at(0) != Verdict::InvalidCondition);
    REQUIRE(trace.at(1) != Verdict::InvalidCondition);
    REQUIRE(trace.at(2) != Verdict::InvalidCondition);
}

TEST_CASE("Diseased is a status like any other", "[idle][status]")
{
    Rule diseased = Always("Restore Magicka");
    diseased.predicate = PredicateKind::Status;
    diseased.statusKind = StatusKind::Diseased;
    const RuleSet idle = Idle({diseased});
    EvalContext ctx;
    Trace trace;

    Snapshot s = Healthy();
    s.inCombat = false;
    REQUIRE_FALSE(Evaluate(idle, s, ctx, &trace).Fired());
    REQUIRE(trace.at(0) == Verdict::ConditionFalse);

    s.traits.Set(StatusKind::Diseased);
    REQUIRE(Evaluate(idle, s, ctx, &trace).Fired());
}
