#include "core/Watchdog.h"

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

} // namespace ft
