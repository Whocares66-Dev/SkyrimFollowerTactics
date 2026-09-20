#include "core/Watchdog.h"

#include <algorithm>

namespace ft
{

std::optional<AfterFight> FightBook::Note(std::vector<Pin> &pins, bool fightingNow)
{
    if (fightingNow && !fighting)
    {
        fighting = true;
        before = pins;
        return std::nullopt;
    }
    if (!fightingNow && fighting)
    {
        fighting = false;
        AfterFight settled = SettleAfterFight(pins, before);
        pins = std::move(before);
        before.clear();
        return settled;
    }
    return std::nullopt;
}

void FightBook::Mirror(PinRequest request, const Holdable &thing, Hand hands, bool moving, bool dualWield)
{
    if (!fighting)
        return;
    [[maybe_unused]] const auto displaced = ApplyRequest(before, request, thing, hands, moving, dualWield);
}

PinVerdict JudgePin(const Pin &pin, const PinSeen &seen, bool fighting, bool castInProgress) noexcept
{
    if (!seen.carried)
        return PinVerdict::Drop;
    if (PutBackNow(pin, seen.on, fighting, castInProgress))
        return PinVerdict::PutBack;
    return PinVerdict::Keep;
}

BanVerdict JudgeBan(const Banned &ban, const BanSeen &seen) noexcept
{
    if (ban.variant && !seen.carried)
        return BanVerdict::Drop;
    if (seen.pinned || !seen.on)
        return BanVerdict::Keep;
    return BanVerdict::TakeOff;
}

WatchPlan PlanWatch(const std::vector<Pin> &pins, std::span<const PinSeen> pinsSeen, const Bans &bans,
                    std::span<const BanSeen> bansSeen, bool fighting, bool castInProgress,
                    std::span<const std::uint32_t> lapsed)
{
    WatchPlan plan;
    for (std::size_t i = 0; i < pins.size() && i < pinsSeen.size(); ++i)
    {
        switch (JudgePin(pins[i], pinsSeen[i], fighting, castInProgress))
        {
        case PinVerdict::Drop:
            plan.dropPins.push_back(i);
            break;
        case PinVerdict::PutBack:
            plan.steps.push_back({WatchAct::PutBack, i, false});
            break;
        case PinVerdict::Keep:
            break;
        }
    }
    // One of our casts holds a hand: a banned thing found on waits, as a
    // hand pin does, rather than being taken out of the cast.
    if (castInProgress)
        return plan;
    for (std::size_t i = 0; i < bans.size() && i < bansSeen.size(); ++i)
    {
        switch (JudgeBan(bans[i], bansSeen[i]))
        {
        case BanVerdict::Drop:
            plan.dropBans.push_back(i);
            break;
        case BanVerdict::TakeOff: {
            const bool afterFight = std::find(lapsed.begin(), lapsed.end(), bans[i].form) != lapsed.end();
            plan.steps.push_back({WatchAct::TakeOff, i, afterFight});
            break;
        }
        case BanVerdict::Keep:
            break;
        }
    }
    return plan;
}

} // namespace ft
