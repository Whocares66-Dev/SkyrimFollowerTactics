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
        // this waits; either starts the settle over. (On the tick the block
        // was raised it is not up yet, so a raised block always waits.)
        if (!seen.blocking || attacking)
        {
            run.waited = true;
            run.steadySince = -1.0;
            return nullptr;
        }
        if (run.blockUpAt < 0.0)
            run.blockUpAt = now;
        if (run.steadySince < 0.0)
            run.steadySince = now;
        if (run.waited && now - run.steadySince < kBashSettleSeconds)
            return nullptr;
        if (!perform(BashCommand::Bash))
        {
            ++run.bashRefusals;
            return nullptr;
        }
        run.sentAt = now;
        run.step = BashStep::Bashing;
        return nullptr;
    }

    if (seen.attack == BashSeen::Attack::Bash)
    {
        if (!run.sawBash)
            run.bashFrom = now;
        run.sawBash = true;
    }
    else if (run.sawBash)
    {
        run.bashEnd = now;
        return "bash made";
    }
    else if (attacking && run.otherAttackState < 0)
        run.otherAttackState = seen.attackState;
    if (now - run.sentAt >= kBashWatchSeconds)
    {
        if (run.sawBash)
            return "watch over, still bashing";
        return run.otherAttackState >= 0 ? "taken, an attack but no bash" : "taken, never bashed";
    }
    return nullptr;
}

} // namespace ft
