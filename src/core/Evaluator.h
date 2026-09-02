#pragma once
// The rule engine. Dragon Age semantics: rules are ordered, evaluated top to
// bottom, and the FIRST rule whose condition holds and whose action is actually
// available wins. If nothing fires, we do nothing and the native combat AI
// carries on untouched -- an empty rule set must behave exactly like vanilla.

#include "Rule.h"
#include "Snapshot.h"

#include <array>

#include <vector>

namespace ft
{

// Why a rule did not fire. Surfaced verbatim in the UI's debug column, which is
// the single most useful feature in the whole editor -- authoring rules against
// an opaque engine without it is guesswork.
enum class Verdict : std::uint8_t
{
    Fired,
    Disabled,
    ConditionFalse,
    OnCooldown,
    ConditionCooldown,
    ActionCooldown,
    GlobalCooldown,
    NoTarget,
    NoResource,
    EffectActive,     // a previous dose is still running
    Unsupported,      // the action cannot be performed on this runtime
    InvalidCondition, // this subject/predicate pair is not answerable at all
    NotReached,       // an earlier rule already fired
};

// The actor a condition matched.
//
// For Self/Player/CurrentTarget this is just that actor. For the group subjects
// it is the specific ally or enemy that satisfied the predicate -- and that is
// the point of returning it: the action then applies to whoever matched,
// without the rule naming them twice.
struct Binding
{
    ActorId id{0};
    bool ok{false};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return ok;
    }
};

struct RuleState
{
    double lastFired{-1.0e9};
};

// One slot per (subject, predicate) pair, for the condition cooldown below.
[[nodiscard]] constexpr std::size_t ConditionSlot(SubjectKind subject, PredicateKind predicate) noexcept
{
    return static_cast<std::size_t>(subject) * static_cast<std::size_t>(PredicateKind::COUNT) +
           static_cast<std::size_t>(predicate);
}

inline constexpr std::size_t kConditionSlots =
    static_cast<std::size_t>(SubjectKind::COUNT) * static_cast<std::size_t>(PredicateKind::COUNT);

struct EvalContext
{
    std::vector<RuleState> ruleStates;

    // Both of these hold a time UNTIL WHICH something is blocked, not the time
    // it last happened. Zero therefore reads as "not blocked", which is what we
    // want at startup -- storing a last-fired time instead needs a sentinel in
    // the distant past, because the game-side clock also starts at zero and
    // every action would look freshly used for the first few seconds of play.

    // Per ACTION, across all rules. Stops one remedy being repeated: two rules
    // that both drink potions must not drink two potions in consecutive ticks.
    std::array<double, static_cast<std::size_t>(ActionKind::COUNT)> actionBlockedUntil{};

    // Per CONDITION -- the (subject, predicate) pair, ignoring the threshold.
    // Stops one *situation* drawing several remedies at once: given
    //     health < 25% -> drink a potion
    //     health < 25% -> cast a healing spell
    //     health < 25% -> eat food
    // the follower should drink, then wait to see whether that fixed it, rather
    // than doing all three in 450 ms. The condition is the problem; the action
    // is the response; you get one response per problem until it has had a
    // chance to work.
    //
    // Note this is only ever set when a rule actually FIRES. A rule that could
    // not act -- no potion in the bag, no target -- blocks nothing, so the next
    // remedy for the same problem is tried immediately, in the same tick.
    std::array<double, kConditionSlots> conditionBlockedUntil{};

    double lastActionAt{-1.0e9};
    double globalCooldown{0.5};
    Capabilities caps{Capabilities::All()};
};

struct Decision
{
    int ruleIndex{-1};
    ActionKind action{ActionKind::None};
    ActorId targetId{0};
    float actionArg{0.0f};

    [[nodiscard]] bool Fired() const noexcept
    {
        return ruleIndex >= 0;
    }
};

using Trace = std::vector<Verdict>;

// Pure. Reads the snapshot, mutates only ctx's bookkeeping when a rule fires.
// Pass a trace to get a per-rule verdict for the debug column.
Decision Evaluate(const RuleSet &rs, const Snapshot &snap, EvalContext &ctx, Trace *trace = nullptr);

// Evaluate one rule's condition and report which actor satisfied it.
//
// For a group subject, when several members satisfy the predicate the binding
// is chosen deterministically and by the predicate's own dimension: the health
// predicates bind the lowest-health member, everything else binds the nearest.
// Ties keep the earlier member in snapshot order. That rule matters -- it is
// what makes "enemy below 30% health" mean the *weakest* such enemy rather than
// an arbitrary one.
Binding EvaluateCondition(const Rule &r, const Snapshot &snap);

// Resolve the action's recipient. `binding` is the condition's result, used
// when the rule targets ActionTargetKind::ConditionSubject.
ActorId ResolveActionTarget(const Rule &r, const Snapshot &snap, Binding binding, bool *ok);

// Exposed for testing and for the UI's live readout.
bool HasResource(ActionKind action, const Snapshot &snap);
bool EffectAlreadyActive(ActionKind action, const Snapshot &snap);

const char *ToString(Verdict v) noexcept;

} // namespace ft
