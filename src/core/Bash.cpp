#include "core/Bash.h"

namespace ft
{

BashState RequestBashAt(double now, bool power) noexcept
{
    BashState state;
    state.power = power;
    state.requestedAt = now;
    return state;
}

const char *AdvanceBash(BashState &run, const BashSeen &seen, double now,
                        const std::function<bool(BashCommand)> &perform)
{
    if (!seen.holder)
        return "holder vanished";
    const bool late = now - run.requestedAt >= kBashDeadlineSeconds;
    const bool attacking = seen.attack != BashSeen::Attack::None;

    if (run.step == BashStep::Ready)
    {
        if (late)
        {
            if (!seen.weaponDrawn)
                return "deadline, weapon never drawn";
            return run.blockRefusals > 0 ? "deadline, block refused" : "deadline, still mid-swing";
        }
        if (!seen.weaponDrawn || attacking)
        {
            run.waited = true;
            run.freeSince = -1.0;
            run.blockOutsBefore = seen.blockOuts;
            return nullptr;
        }
        if (run.freeSince < 0.0)
            run.freeSince = now;
        if (seen.blocking)
            run.alreadyBlocking = true;
        else
        {
            if (run.blockAskedAt < 0.0)
                run.blockAskedAt = now;
            run.blockOutsBefore = seen.blockOuts;
            run.raised = perform(BashCommand::RaiseBlock);
            if (!run.raised)
            {
                ++run.blockRefusals;
                run.waited = true;
                return nullptr;
            }
        }
        run.step = BashStep::Blocking;
    }

    if (run.step == BashStep::Blocking)
    {
        if (late)
        {
            if (run.blockUpAt < 0.0)
                return "deadline, block never up";
            return run.bashRefusals > 0 ? "deadline, bash refused from the block" : "deadline, never steady";
        }
        // The follower's own AI may swing again, or drop the block, while
        // this waits; either waits for the block to be ready again. (On the
        // step the block was raised it is not up yet, so a raised block
        // always waits.)
        if (!seen.blocking || attacking)
        {
            run.waited = true;
            run.steadySince = -1.0;
            run.blockOutsBefore = seen.blockOuts;
            return nullptr;
        }
        if (run.blockUpAt < 0.0)
            run.blockUpAt = now;
        // After a wait the bash is taken from the block once its animation
        // is ready, its blockStartOut heard since the wait: the tree
        // resolves no bash from a block still coming up, though the actor
        // reads as blocking from its start (no bash in eight taken the
        // moment it did, 2026-09-15). A 0.25 s settle stood in for this
        // event until 2026-09-24. A block up and ready at the request is
        // taken from at once.
        if (run.waited && seen.blockOuts <= run.blockOutsBefore)
            return nullptr;
        if (run.steadySince < 0.0)
            run.steadySince = now;
        if (!perform(BashCommand::Bash))
        {
            ++run.bashRefusals;
            return nullptr;
        }
        run.sentAt = now;
        run.bashStopsBefore = seen.bashStops;
        run.step = BashStep::Bashing;
        return nullptr;
    }

    // The bash's own end, bashStop, heard since it was taken: made. Its
    // attack state is short, and falls between the events that step this
    // (2026-09-24: two power bashes made, hit and paid for, neither seen
    // in it); it is kept for the report where a step does land in it.
    if (seen.attack == BashSeen::Attack::Bash)
    {
        if (!run.sawBash)
            run.bashFrom = now;
        run.sawBash = true;
    }
    else if (attacking && run.otherAttackState < 0)
        run.otherAttackState = seen.attackState;
    if (seen.bashStops > run.bashStopsBefore)
    {
        run.sawBash = true;
        run.bashEnd = now;
        return "bash made";
    }
    if (now - run.sentAt >= kBashWatchSeconds)
    {
        if (run.sawBash)
            return "watch over, still bashing";
        return run.otherAttackState >= 0 ? "taken, an attack but no bash" : "taken, never bashed";
    }
    return nullptr;
}

} // namespace ft
