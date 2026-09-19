#pragma once
// One actor's turn, in order: a request that has finished puts its action
// back on cooldown, the tick chooses a list, a snapshot is built for it,
// the rules are evaluated, and the one action decided is dispatched. The
// game reads the actor and performs the action (game/Tactics.cpp, Tick);
// the order itself is here, so the follower's turn and the player's are
// one function and the tests drive the same one rather than an imitation
// of it. No Skyrim.
//
// The snapshot is asked for only when a list is to be evaluated: out of a
// fight, with no idle rules, an actor costs no inventory scan at all.

#include "Evaluator.h"
#include "Tick.h"

#include <functional>
#include <optional>

namespace ft
{

// What the game's dispatch answers (game/Actions.h, ActionResult): done,
// asked of the AI and still to resolve, or refused.
enum class ActionOutcome : std::uint8_t
{
    Performed,
    Requested,
    Failed
};

// What the tick keeps for one actor between turns.
struct ActorRun
{
    ActorTick tick;
    EvalContext eval;
    // The requested action in flight -- a cast, a shout, a power attack, a
    // bash -- whose cooldown starts again when it is over.
    std::optional<Decision::Step> inFlight;
};

// What the game read of the actor this turn.
struct TickFacts
{
    ActorTick::Now now;
    Capabilities caps;
    // One of ours is still in the air: the request in flight is not over.
    bool busy{false};
};

struct TickResult
{
    TickPlan plan;         // the list evaluated and its edges; none when nothing ran
    bool completed{false}; // a request ended this turn and its cooldown restarted
    Snapshot snapshot;     // what the evaluation saw, for the log
    Decision decision;     // what it decided
    [[nodiscard]] bool Fired() const noexcept
    {
        return decision.Fired();
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return plan.list.has_value();
    }
};

// The turn up to the decision: the finished request's cooldown, the list,
// the snapshot (asked for only when a list is to be evaluated, with the
// edges set here) and the evaluation. `trace` and `actionTrace`, when
// given, are filled for the caller's report. The caller then performs the
// action the decision names and says what came of it.
[[nodiscard]] TickResult DecideTurn(ActorRun &run, const ActorRules &rules, const TickFacts &facts, double now,
                                    const std::function<Snapshot(Moment)> &snapshot, Trace *trace = nullptr,
                                    ActionTrace *actionTrace = nullptr);

// What came of the action: a request stays in flight until it is over,
// and its cooldown starts then; anything else is done with.
void NoteOutcome(ActorRun &run, const Decision &decision, ActionOutcome outcome);

} // namespace ft
