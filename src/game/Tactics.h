#pragma once
// The Phase 1 spike: find followers, evaluate one hardcoded rule against each,
// and act on the result.
//
// Scope is deliberately narrow (docs/PLAN.md section 4, Phase 1):
//   if a follower's health drops below 50%, drink the best health potion,
//   at most once every 10 seconds.
//
// No UI, no JSON, no per-follower profiles. This exists to retire one risk --
// whether an NPC can be made to reliably consume a potion -- because if that
// cannot be done, the marquee rule does not work and the concept needs
// rethinking.

#include "core/Evaluator.h"

#include <string>
#include <vector>

namespace ft::game
{

// A copy of what the engine last decided about one follower.
//
// The UI renders on the render thread while the tick runs on the game thread,
// so nothing hands out a pointer into live state -- callers get a snapshot they
// own. Copying a handful of small vectors once per UI frame is far cheaper than
// the alternative of holding a lock across rendering.
struct FollowerView
{
    ft::ActorId id{0};
    // Plain display name, no FormID. The ID is a debugging detail and belongs
    // in the log, where Describe() still emits it -- on screen it is noise the
    // player can get from the console if they ever need it.
    std::string name;
    ft::Snapshot snapshot;
    ft::Trace trace; // per-rule verdict: the debug column
    ft::Decision decision;
    double lastEvaluatedAt{0.0};

    // Whether this follower's rules were actually evaluated this tick.
    //
    // False out of combat, where we deliberately do only the cheap part: read
    // the actor values so the UI can show who is under tactics control, and
    // skip both the inventory scan and the evaluation. The trace and potion
    // counts are meaningless then, and the UI says so rather than showing a
    // stale verdict or a confident zero.
    bool evaluated{false};
    bool inCombat{false};

    // This follower's own on/off switch, independent of the global one and of
    // each rule's own `enabled`. Turning a follower off must never touch the
    // individual rules -- the player's choices there are theirs, and silently
    // rewriting them would mean re-authoring the list after every toggle.
    bool tacticsEnabled{true};

    // Display only -- not rule inputs, so they stay out of Snapshot, which is
    // the RE::-free contract the evaluator reads. All three are cheap reads and
    // are filled in and out of combat alike.
    std::uint16_t level{0};
    float carriedWeight{0.0f};
    float carryCapacity{0.0f};
};

struct CostStats
{
    double avgUs{0.0};
    double maxUs{0.0};
    std::uint64_t samples{0};
};

// Everything the UI needs, all copied. Includes followers who are NOT fighting:
// tactics are authored before a fight, so the panel has to show them then.
[[nodiscard]] std::vector<FollowerView> ObserveFollowers();
[[nodiscard]] CostStats ObserveCost();

// The rule set being evaluated. Hardcoded in Phase 1, so this is a reference to
// a static; it becomes per-follower and mutable once profiles land.
[[nodiscard]] const ft::RuleSet &ActiveRuleSet();

// Start ticking. Safe to call once, after kDataLoaded.
void Install();

// Enable/disable at runtime without unregistering the tick. Global: applies to
// every follower.
void SetEnabled(bool enabled);
[[nodiscard]] bool IsEnabled();

// Per follower, on top of the global switch. A follower is evaluated only when
// both are on. Defaults to on for anyone not explicitly turned off.
void SetFollowerEnabled(ft::ActorId id, bool enabled);
[[nodiscard]] bool IsFollowerEnabled(ft::ActorId id);

} // namespace ft::game
