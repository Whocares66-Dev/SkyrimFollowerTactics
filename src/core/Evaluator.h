#pragma once
// The rule engine. Dragon Age semantics: rules are ordered, evaluated top to
// bottom, and the FIRST rule whose condition holds and whose action is actually
// available wins. If nothing fires, we do nothing and the native combat AI
// carries on untouched -- an empty rule set must behave exactly like vanilla.

#include "Rule.h"
#include "Snapshot.h"

#include <vector>

namespace ft {

// Why a rule did not fire. Surfaced verbatim in the UI's debug column, which is
// the single most useful feature in the whole editor -- authoring rules against
// an opaque engine without it is guesswork.
enum class Verdict : std::uint8_t {
    Fired,
    Disabled,
    ConditionFalse,
    OnCooldown,
    GlobalCooldown,
    NoTarget,
    NoResource,
    Unsupported,
    NotReached,  // an earlier rule already fired
};

struct RuleState {
    double lastFired{-1.0e9};
};

struct EvalContext {
    std::vector<RuleState> ruleStates;
    double                 lastActionAt{-1.0e9};
    double                 globalCooldown{0.5};
    Capabilities           caps{Capabilities::All()};
};

struct Decision {
    int        ruleIndex{-1};
    ActionKind action{ActionKind::None};
    ActorId    targetId{0};
    float      actionArg{0.0f};

    [[nodiscard]] bool Fired() const noexcept { return ruleIndex >= 0; }
};

using Trace = std::vector<Verdict>;

// Pure. Reads the snapshot, mutates only ctx's bookkeeping when a rule fires.
// Pass a trace to get a per-rule verdict for the debug column.
Decision Evaluate(const RuleSet& rs, const Snapshot& snap, EvalContext& ctx,
                  Trace* trace = nullptr);

// Exposed for testing and for the UI's live condition readout.
bool     ConditionHolds(const Rule& r, const Snapshot& snap);
ActorId  ResolveTarget(TargetKind kind, const Snapshot& snap, bool* ok);
bool     HasResource(ActionKind action, const Snapshot& snap);

const char* ToString(Verdict v) noexcept;

}  // namespace ft
