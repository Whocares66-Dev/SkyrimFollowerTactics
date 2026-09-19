#include "core/Coordinator.h"

namespace ft
{

TickResult DecideTurn(ActorRun &run, const ActorRules &rules, const TickFacts &facts, double now,
                      const std::function<Snapshot(Moment)> &snapshot, Trace *trace, ActionTrace *actionTrace)
{
    TickResult result;
    // A request ended since the last turn: its cooldown runs from the end,
    // not from the decision (MinimumCooldown in Rule.h). Before anything
    // else, so the rule that asked can fire again on this very turn once
    // the cooldown has run.
    if (run.inFlight && !facts.busy)
    {
        RestartCooldown(run.eval, run.inFlight->action, run.inFlight->target, now);
        run.inFlight.reset();
        result.completed = true;
    }

    result.plan = run.tick.Plan(run.eval, facts.now);
    if (!result.plan)
        return result;

    run.eval.caps = facts.caps;
    result.snapshot = snapshot(*result.plan.list);
    result.snapshot.combatBegan = result.plan.began;
    result.snapshot.combatEnded = result.plan.ended;
    result.decision = Evaluate(rules.Of(*result.plan.list), result.snapshot, run.eval, trace, actionTrace);
    return result;
}

void NoteOutcome(ActorRun &run, const Decision &decision, ActionOutcome outcome)
{
    // A cast, a shout, a power attack or a bash is only asked for; what
    // came of it follows when the lease or the run is over, and its
    // cooldown starts then.
    if (decision.Fired() && outcome == ActionOutcome::Requested)
        run.inFlight = *decision.step;
}

} // namespace ft
