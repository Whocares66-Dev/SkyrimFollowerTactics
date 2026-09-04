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
#include "game/Inventory.h"
#include "game/Magic.h"
#include "game/Sensors.h"

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
    ft::Trace trace;             // per-rule verdict: the debug column
    ft::ActionTrace actionTrace; // per-action verdicts: the column's tooltip
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
    // The spells this follower can be told to cast or equip, sorted by name.
    // Lives on the view rather than in Snapshot because it is menu content, not
    // a rule input -- the evaluator only ever compares FormIDs.
    std::vector<SpellOption> spells;
    // The potions she carries, for the drink menu. Same reasoning.
    std::vector<PotionOption> potions;

    std::uint16_t level{0};
    float carriedWeight{0.0f};
    float carryCapacity{0.0f};
    // The Character and Skills tabs' sections, worded on the game thread.
    std::vector<SheetSection> sheet;
    std::vector<SheetSection> skills;
    // The Inventory tab: everything she carries, sorted by name.
    std::vector<InventoryItem> inventory;
    // The Magic tab: spells, powers and shouts, sorted by name.
    std::vector<MagicEntry> magic;
    // The Tactics tab's Combat Style section.
    std::vector<SheetSection> combatStyle;
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

// This follower's rules, as a copy.
//
// Copy in, copy out. Rule sets hold a handful of rules, so copying is cheap,
// and it removes a whole class of problem: the UI edits its own copy across as
// many frames as it likes and writes the result back, with no partial state
// visible to the tick and no lock held across rendering.
//
// Only the UI writes. The tick reads. That is what makes read-modify-write safe
// here without a version check.
[[nodiscard]] ft::RuleSet GetRules(ft::ActorId id);
void SetRules(ft::ActorId id, ft::RuleSet rules);

// The rules a follower starts with, before anyone edits them.
[[nodiscard]] const ft::RuleSet &DefaultRuleSet();

// Rebuild and publish one follower's view now, out of turn: for a request
// that has just changed her, so the panel answers before the next tick --
// which the frozen clock holds while the panel is open.
void PublishFollower(RE::Actor *actor);

// Why the world's clock is stopped, if it is.
//
// Rules are gated on time running, not on any menu being closed -- those are
// different questions, and only the first is the one a tactic cares about.
// Firing into a frozen world is how a half-written rule drank potions while it
// was still being edited.
//
// Both the tick and the panel read this, so what the panel reports is by
// construction what the tick actually did, rather than a second opinion that
// can drift from it. See ReadClock() for why it takes two signals.
struct ClockState
{
    bool pausedMenu{false};  // inventory, map, journal, settings, console
    bool frozenClock{false}; // our own panel, with FreezeTimeOnMenu = true

    [[nodiscard]] bool stopped() const
    {
        return pausedMenu || frozenClock;
    }
};

[[nodiscard]] ClockState ReadClock();

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
