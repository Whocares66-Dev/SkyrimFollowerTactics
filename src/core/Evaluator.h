#pragma once
// The rule engine. Dragon Age semantics: rules are ordered, evaluated top to
// bottom, and the FIRST rule whose condition holds and whose action is actually
// available wins. If nothing fires, we do nothing and the native combat AI
// carries on untouched -- an empty rule set must behave exactly like vanilla.

#include "Rule.h"
#include "Snapshot.h"

#include <optional>
#include <vector>

namespace ft
{

// Why a rule did not fire. Authoring rules against an opaque engine is
// guesswork without it, so every evaluation says, per rule, and the log
// carries it (dev/EVENTS.md).
enum class Verdict : std::uint8_t
{
    Fired,
    Disabled,
    ConditionFalse,
    ActionCooldown,
    NoTarget,
    NoResource,
    NotInCombat,      // an Attack or a blow with no fight on: there is no one to be pointed at
    NothingToPoison,  // an Apply rule with no weapon in hand that takes a poison
    NothingToCharge,  // a Charge rule with no enchanted weapon in hand
    CannotAfford,     // knows the spell, cannot pay for it right now
    CannotDualCast,   // a dual cast of a spell the snapshot does not mark dualable
    NoMeleeWeapon,    // a blow with nothing in hand for it: a swing without a blade, a bash without a shield
    NoPerk,           // a blow the Settings page asks a perk for, which this follower has not got
    NoStamina,        // a blow the follower cannot pay for right now
    OutOfReach,       // a blow at an enemy further than it reaches
    EffectActive,     // a previous dose is still running; or the thing is already pinned
    AboveSkill,       // a spell above the follower's skill: neither cast nor pinned, so cast and equip agree
    Outranked,        // a rule above holds the hand or slot this would take
    Unsupported,      // the action cannot be performed on this runtime
    Busy,             // it can, but not this evaluation: the follower is mid-cast on one of ours
    Casting,          // a cast rule, while the follower is mid-cast on a spell of their own: it waits
    Recovering,       // a shout rule, while the voice is still recovering from the last shout: it waits
    PowerUsed,        // a greater power used today: once a day, until the day turns
    InvalidCondition, // this subject/predicate pair is not answerable at all
    Queued,           // an edge rule whose list waits behind another's on the same edge
    NotReached,       // an earlier rule already fired
};

// The gem a charge policy spends into a weapon short by `missing`. Strongest
// is the largest that would not overfill it, or the smallest carried when
// every one would; weakest the smallest carried. Zero for no gems.
[[nodiscard]] std::uint32_t ChooseSoulGem(const std::vector<Snapshot::SoulGemView> &gems, float missing,
                                          bool strongest) noexcept;

// The actor a condition matched.
//
// For Self and Player this is just that actor. For the group subjects it is
// the specific ally, enemy or corpse that satisfied the predicate -- and
// that is the point of returning it: the action then applies to whoever
// matched, without the rule naming them twice.
struct Binding
{
    ActorId id{0};
    bool ok{false};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return ok;
    }
};

// The thing an action takes: a named one's own form, or for a Strongest /
// Weakest policy the one chosen from the stock by its effect, or for a
// Charge policy the gem sized to the weapon in need -- 0 for none. The
// Decision's step carries the resolved form, so the game side has only to
// consume it.
[[nodiscard]] std::uint32_t ChosenForm(const Action &a, const Snapshot &snap);

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
    // health potion does not block a magicka potion; healing themself does not
    // block healing the player; Oakflesh does not block a heal. What it DOES
    // block is every rule, wherever it sits in the list, that would do the
    // same thing to the same actor before the first has had time to show.
    //
    // This is the only timer there is: nothing is keyed by the rule or by
    // the condition. Why, and what it means when several rules answer one
    // situation, is at MinimumCooldown in Rule.h.
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

    Capabilities caps;

    // A rule's list of actions in progress. A rule commits when its first
    // action is done; from then on an action that cannot be done yet is
    // waited for, tick after tick, and no other rule is evaluated until the
    // list is through. Held as a copy of the rule, so editing or reordering
    // the rules mid-list changes nothing already begun, and the log names the
    // rule that is acting rather than whatever now sits at its index.
    // Dropped when a fight ends and when a new one begins.
    struct Sequence
    {
        int ruleIndex{-1};
        Rule rule;
        ActorId subject{0}; // whom the condition bound when the list began
        ActorId target{0};
        std::size_t next{0}; // the first action not yet done

        [[nodiscard]] bool Active() const noexcept
        {
            return next < rule.actions.size();
        }
    };
    Sequence pending;
    // The lists that follow it, in order: the edge of a fight puts every
    // Combat start (or Combat end) rule's list here at once, since the edge
    // holds for one evaluation and a rule not begun on it would never be.
    // Each is committed from its first action -- the edge is not coming
    // back -- and one starts as the one before it is through.
    std::vector<Sequence> queued;

    // Is a list being run, or waiting to be? Out of a fight this is the
    // Combat end lists, and the only reason to evaluate at all.
    [[nodiscard]] bool InProgress() const noexcept
    {
        return pending.Active() || !queued.empty();
    }
};

// What to do this tick: one action of one rule, with the actor it applies
// to -- a rule's list runs one action per tick (Run). Carried through from
// the rule so dispatch needs only the Decision; the form is resolved here,
// and the game side never chooses one.
struct Decision
{
    struct Step
    {
        Action action;
        ActorId target{0};
        // Whom the rule's condition bound: the follower, the player, or the
        // ally, enemy or corpse it matched. 0 for Corpse: None, whose binding
        // is the follower themself and would read as a corpse.
        ActorId subject{0};
    };

    int ruleIndex{-1};
    // The rule as its list began, kept though the rules are edited or
    // reordered under a list in progress.
    Rule rule;
    std::optional<Step> step;

    [[nodiscard]] bool Fired() const noexcept
    {
        return step.has_value();
    }

    // The step's parts, for the log and the tests; None/0 with no step.
    [[nodiscard]] ActionKind action() const noexcept
    {
        return step ? step->action.kind : ActionKind::None;
    }
    [[nodiscard]] std::uint32_t actionForm() const noexcept
    {
        return step ? step->action.form : 0;
    }
    [[nodiscard]] Hand hand() const noexcept
    {
        return step ? step->action.hand : Hand::None;
    }
    [[nodiscard]] ActorId targetId() const noexcept
    {
        return step ? step->target : 0;
    }
    [[nodiscard]] ActorId subjectId() const noexcept
    {
        return step ? step->subject : 0;
    }
};

using Trace = std::vector<Verdict>;
// Per rule, per action: why each action did or did not happen. NotReached
// for an action after one still being waited for, and for every action of
// a rule that was not reached.
using ActionTrace = std::vector<std::vector<Verdict>>;

// Pure. Reads the snapshot, mutates only ctx's bookkeeping when a rule fires.
// Pass a trace to get a verdict per rule, and an action trace for one per
// action. The list's moment (RuleSet::moment) says when it decides: the
// combat list in a fight and on its edges, the idle list out of one. One
// context serves an actor's two lists -- a cooldown is the action's, not
// the list's, and a list in progress is whichever began last -- so the
// caller evaluates one list per tick, never both.
Decision Evaluate(const RuleSet &rs, const Snapshot &snap, EvalContext &ctx, Trace *trace = nullptr,
                  ActionTrace *actionTrace = nullptr);

// Every action's availability now, rule by rule, with nothing decided and
// nothing changed: what the panel greys an action by and says why on its
// hover. The target is the one the rule's condition binds now, or nobody
// where it does not hold, which the target-keyed checks read as such.
[[nodiscard]] ActionTrace ProbeAvailability(const RuleSet &rs, const Snapshot &snap, const EvalContext &ctx);

// Start an action's cooldown again from `now`: the moment it is over. A
// drink or an equip is over when it is dispatched, and Evaluate's own stamp
// stands; a cast, a shout, a power attack or a bash is over when its lease
// or its request ends, and the game side calls this then, so the next
// firing waits for the result and not only for the decision.
void RestartCooldown(EvalContext &ctx, const Action &a, ActorId target, double now);

// Evaluate one rule's condition and report which actor satisfied it.
//
// For a group subject, when several members satisfy the predicate the binding
// is chosen deterministically and by the predicate's own dimension: the health
// predicates bind the lowest-health member, everything else binds the nearest.
// Ties keep the earlier member in snapshot order. That rule matters -- it is
// what makes "enemy below 30% health" mean the *weakest* such enemy rather than
// an arbitrary one. A negated condition binds the nearest member the plain one
// does not hold of: "NOT enemy below 30%" names no direction to rank by.
Binding EvaluateCondition(const Rule &r, const Snapshot &snap);

// Resolve the action's recipient. `binding` is the condition's result, used
// when the rule aims at the ally or enemy the condition matched.
ActorId ResolveActionTarget(const Rule &r, const Snapshot &snap, Binding binding, bool *ok);

const char *ToString(Verdict v) noexcept;

// The same verdict, worded for the action it happened to.
//
// ToString is generic and therefore wrong half the time: an equip rule that is
// already satisfied logged "previous dose still active", which sent someone
// looking for a potion that was never involved. Two verdicts mean genuinely
// different things depending on the action, and the wording has to follow or
// the log misdirects exactly when it is being read most carefully.
[[nodiscard]] const char *Explain(Verdict v, ActionKind action) noexcept;

// The verdict as the events log names it: "condition-false", "no-resource".
// A key a query is written against, so it never follows a change of wording.
[[nodiscard]] const char *WireName(Verdict v) noexcept;

// Which rules to report after an evaluation, given the verdicts last
// reported, which it updates. Not reached says nothing and keeps the last
// word; a fire updates it silently, since rule.fired says so; any other
// verdict that differs is reported. A list of another length -- the rules
// changed -- starts again.
[[nodiscard]] std::vector<std::size_t> VerdictChanges(Trace &reported, const Trace &now);

} // namespace ft
