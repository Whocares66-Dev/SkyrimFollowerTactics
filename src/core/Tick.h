#pragma once
// What one tick decides for one actor before any snapshot is built: which
// of the two lists to evaluate, and whether this is the fight's first
// evaluation or the farewell one after it. The game side reads the actor
// and the switches and hands the facts here (game/Tactics.cpp, Tick); the
// choice, and the book-keeping of the fight's edges, are decided here,
// where they are tested. No Skyrim.

#include "Evaluator.h"
#include "Rule.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace ft
{

// An actor's two lists, read together: what one tick decides from and
// evaluates against. Read once per tick, so the spells priced for the
// snapshot and the rules evaluated are one version of the list -- the
// panel replaces a list from the render thread at any moment.
struct ActorRules
{
    RuleSet combat;
    RuleSet idle;

    [[nodiscard]] const RuleSet &Of(Moment moment) const noexcept
    {
        return moment == Moment::Idle ? idle : combat;
    }
};

// The spells the actor's rules cast, from both lists, each once: what the
// snapshot prices. A rule added in the panel is in the next tick's lists,
// so a newly named spell is priced on the tick it could first fire.
[[nodiscard]] std::vector<std::uint32_t> SpellsNamedBy(const ActorRules &rules);

// What a tick does for an actor: the list to evaluate, or none, and the
// edges that evaluation is for.
struct TickPlan
{
    std::optional<Moment> list;
    bool began{false}; // the fight's first evaluation
    bool ended{false}; // the one farewell evaluation after it

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return list.has_value();
    }
};

// An actor's standing between ticks.
struct ActorTick
{
    // In combat as of the last evaluation. The edges of a fight are read
    // against this, so an edge is used up only by an evaluation: held down
    // or switched off through it, the actor still owes the fight its first
    // evaluation, or the farewell one -- a heal-after-the-fight rule is
    // exactly what someone just up from bleeding out needs.
    bool fighting{false};
    // The list evaluated last, and so whose a list in progress is: one
    // context serves both, and a tick hands the actor to one list.
    Moment moment{Moment::Combat};

    // What the tick reads of the actor.
    struct Now
    {
        bool fighting{false};
        // Nothing can be performed: bleeding out, the player somewhere an
        // automatic cast is wrong, the switch over all lists off. Nothing
        // is evaluated and the edge is kept for when they are free.
        bool held{false};
        // Each list's own switch. A list switched off is silenced without
        // being lost, and silenced is silenced: it is not owed an edge, and
        // a list of its own in progress is dropped rather than left waiting
        // for the switch, blocking the other list.
        bool combatEnabled{true};
        bool idleEnabled{true};
        // Out of a fight the idle list is the only thing decided, so an
        // actor with no idle rules costs no snapshot at all.
        bool idleHasRules{false};
    };

    // The combat list in a fight, on its farewell, and while a list of its
    // own is in progress -- the Combat end lists run on after the fight,
    // one action per tick. Otherwise the idle list, while it has rules or
    // a list of its own in progress. The fight's first tick goes to the
    // combat list whatever the idle list was doing; the evaluator drops
    // the idle list's sequence on that edge. When a list is returned the
    // actor is handed to it: the standing here moves on, and the caller
    // evaluates.
    [[nodiscard]] TickPlan Plan(EvalContext &ctx, const Now &now) noexcept;
};

} // namespace ft
