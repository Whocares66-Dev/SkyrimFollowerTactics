#include "core/Lease.h"

namespace ft
{

LeaseState ArmLease(double now, double window, bool sustained, float sustain) noexcept
{
    LeaseState state;
    state.armedAt = now;
    state.until = now + window;
    state.sustained = sustained;
    state.sustain = sustain;
    return state;
}

LeaseStep AdvanceCast(LeaseState &state, const LeaseSeen &seen, LeaseKind kind, double now) noexcept
{
    LeaseStep step;
    if (!seen.holder)
    {
        step.finish = "holder vanished";
        return step;
    }
    if (seen.running && !state.seenRunning)
    {
        state.seenRunning = true;
        step.pickedUp = true;
    }

    // The stream's start moves the deadline to its end, once. Not a max:
    // the stream is what was asked for, and a short one ends sooner than
    // the pick-up window would have.
    if (state.sustained && !state.streaming && seen.fired)
    {
        state.streaming = true;
        state.until = now + state.sustain + kStreamGraceSeconds;
        step.streamExtended = true;
    }
    // The cast begun -- the animation started -- is worth a few seconds
    // more, once, never fewer.
    if (!state.extended && seen.begun)
    {
        state.extended = true;
        state.until = (std::max)(state.until, now + kBeginCastSeconds);
        step.beginExtended = true;
    }

    // One finish, the first that holds: a fire before an exit before the
    // clock.
    if (!state.sustained && seen.fired)
    {
        step.finish = kind == LeaseKind::Power   ? "power fired"
                      : kind == LeaseKind::Shout ? "shout fired"
                                                 : "spell fired";
        step.fired = true;
    }
    else if (state.sustained && seen.stopped)
        step.finish = "stream ended";
    else if (state.sustained && seen.targetDead)
        step.finish = "target dead";
    else if (state.seenRunning && !seen.running)
        step.finish = "package ended";
    else if (now >= state.until)
    {
        if (!state.seenRunning)
            step.finish = "deadline, AI never picked it up";
        else if (state.streaming)
            step.finish = "deadline, stream still running";
        else if (kind != LeaseKind::Spell)
            step.finish = "deadline, shout never ended";
        else
            step.finish = "deadline, never cast";
    }
    return step;
}

} // namespace ft

namespace ft
{

std::optional<std::size_t> ChooseStack(std::span<const StackSeen> places) noexcept
{
    std::optional<std::size_t> fullest;
    std::size_t most = 0;
    for (std::size_t i = 0; i < places.size(); ++i)
    {
        const StackSeen &place = places[i];
        if (place.holdsRunning)
            return i;
        if (place.size > most)
        {
            most = place.size;
            fullest = i;
        }
    }
    return fullest;
}

} // namespace ft
