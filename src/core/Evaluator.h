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
    ActionCooldown,
    NoTarget,
    NoResource,
    NothingToPoison,  // an Apply rule with no weapon in hand that takes a poison
    NothingToCharge,  // a Charge rule with no enchanted weapon in hand
    CannotAfford,     // knows the spell, cannot pay for it right now
    CannotDualCast,   // a dual cast of a spell the follower cannot dual cast: no perk for its school
    EffectActive,     // a previous dose is still running; or the thing is already pinned
    AboveSkill,       // a spell above the follower's skill: neither cast nor pinned, so cast and equip agree
    Outranked,        // a rule above holds the hand or slot this would take
    Unsupported,      // the action cannot be performed on this runtime
    Busy,             // it can, but not this evaluation: its resource pool is exhausted
    Casting,          // a cast rule, while the follower is mid-cast on a spell of their own: it waits
    Recovering,       // a shout rule, while the voice is still recovering from the last shout: it waits
    InvalidCondition, // this subject/predicate pair is not answerable at all
    NotReached,       // an earlier rule already fired
};

// The actor a condition matched.
//
// For Self/Player/CurrentTarget this is just that actor. For the group subjects
// it is the specific ally or enemy that satisfied the predicate -- and that is
// the point of returning it: the action then applies to whoever matched,
// without the rule naming them twice.
// The gem a charge policy spends into a weapon short by `missing`. Strongest
// is the largest that would not overfill it, or the smallest carried when
// every one would; weakest the smallest carried. Zero for no gems.
[[nodiscard]] std::uint32_t ChooseSoulGem(const std::vector<Snapshot::SoulGemView> &gems, float missing,
                                          bool strongest) noexcept;

struct Binding
{
    ActorId id{0};
    bool ok{false};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return ok;
    }
};

// The bottle an action takes: a named one's own form, or for a Strongest /
// Weakest policy the one chosen from the stock by its effect -- 0 for none.
// The Decision's steps carry the resolved form, so the game side has only
// to consume it.
[[nodiscard]] std::uint32_t ChosenForm(const Action &a, const PotionStock &stock);

struct EvalContext
{
    // Both of these hold a time UNTIL WHICH something is blocked, not the time
    // it last happened. Zero therefore reads as "not blocked", which is what we
    // want at startup -- storing a last-fired time instead needs a sentinel in
    // the distant past, because the game-side clock also starts at zero and
    // every action would look freshly used for the first few seconds of play.

    // Per ACTION, across all rules. Stops one remedy being repeated: two rules
    // that both drink potions must not drink two potions in consecutive ticks.
    // What exactly is on cooldown: the action, the spell it casts (zero for
    // an action without one) and the actor it was applied to. Drinking a
    // health potion does not block a magicka potion; healing herself does not
    // block healing the player; Oakflesh does not block a heal. What it DOES
    // block is every rule, wherever it sits in the list, that would do the
    // same thing to the same actor before the first has had time to show.
    struct ActionKey
    {
        ActionKind action{ActionKind::None};
        std::uint32_t form{0};
        ActorId target{0};
        // The policy's effect: "drink the strongest Restore Health" and
        // "... Resist Fire" are two actions, each on its own cooldown.
        std::string effect;

        [[nodiscard]] bool operator==(const ActionKey &o) const noexcept
        {
            return action == o.action && form == o.form && target == o.target && effect == o.effect;
        }
    };

    struct Blocked
    {
        ActionKey key;
        double until{0.0};
    };
    std::vector<Blocked> blocked;

    [[nodiscard]] double BlockedUntil(const ActionKey &key) const noexcept
    {
        for (const auto &b : blocked)
            if (b.key == key)
                return b.until;
        return 0.0;
    }

    void Block(const ActionKey &key, double until)
    {
        for (auto &b : blocked)
            if (b.key == key)
            {
                b.until = until;
                return;
            }
        blocked.push_back({key, until});
    }

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

    Capabilities caps{Capabilities::All()};

    // A rule's list of actions in progress. A rule commits when its first
    // action is done; from then on an action that cannot be done yet is
    // waited for, tick after tick, and no other rule is evaluated until the
    // list is through. Held as a copy of the actions, so editing the rules
    // mid-list changes nothing already begun. Dropped when a fight ends and
    // when a new one begins.
    struct Sequence
    {
        int ruleIndex{-1};
        ActionTargetKind aimedAt{ActionTargetKind::Self};
        ActorId target{0};
        std::vector<Action> actions;
        std::size_t next{0}; // the first action not yet done

        [[nodiscard]] bool Active() const noexcept
        {
            return next < actions.size();
        }
    };
    Sequence pending;
};

// What to do this tick: the actions of one rule that can be done now, in
// order, each with the actor it applies to. Carried through from the rule so
// dispatch needs only the Decision; core never resolves a form.
struct Decision
{
    struct Step
    {
        Action action;
        ActorId target{0};
    };

    int ruleIndex{-1};
    std::vector<Step> steps;

    [[nodiscard]] bool Fired() const noexcept
    {
        return !steps.empty();
    }

    // The first step's parts, for the log and the tests; None/0 with no step.
    [[nodiscard]] ActionKind action() const noexcept
    {
        return steps.empty() ? ActionKind::None : steps.front().action.kind;
    }
    [[nodiscard]] std::uint32_t actionForm() const noexcept
    {
        return steps.empty() ? 0 : steps.front().action.form;
    }
    [[nodiscard]] Hand hand() const noexcept
    {
        return steps.empty() ? Hand::None : steps.front().action.hand;
    }
    [[nodiscard]] ActorId targetId() const noexcept
    {
        return steps.empty() ? 0 : steps.front().target;
    }
};

using Trace = std::vector<Verdict>;
// Per rule, per action: why each action did or did not happen, for the
// status tooltip. NotReached for an action after one still being waited
// for, and for every action of a rule that was not reached.
using ActionTrace = std::vector<std::vector<Verdict>>;

// Pure. Reads the snapshot, mutates only ctx's bookkeeping when a rule fires.
// Pass a trace to get a per-rule verdict for the debug column, and an action
// trace for the per-action breakdown.
Decision Evaluate(const RuleSet &rs, const Snapshot &snap, EvalContext &ctx, Trace *trace = nullptr,
                  ActionTrace *actionTrace = nullptr);

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
// when the rule aims at the ally or enemy the condition matched.
ActorId ResolveActionTarget(const Rule &r, const Snapshot &snap, Binding binding, bool *ok);

// Exposed for testing and for the UI's live readout.
bool HasResource(const Action &action, const Snapshot &snap);
bool EffectAlreadyActive(const Action &action, const Snapshot &snap);

const char *ToString(Verdict v) noexcept;

// The same verdict, worded for the action it happened to.
//
// ToString is generic and therefore wrong half the time: an equip rule that is
// already satisfied logged "previous dose still active", which sent someone
// looking for a potion that was never involved. Two verdicts mean genuinely
// different things depending on the action, and the wording has to follow or
// the log misdirects exactly when it is being read most carefully.
[[nodiscard]] const char *Explain(Verdict v, ActionKind action) noexcept;

} // namespace ft
