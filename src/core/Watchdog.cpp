#include "core/Watchdog.h"

#include <algorithm>

namespace ft
{

std::optional<Settled> FightBook::Note(std::vector<Pin> &pins, bool fightingNow)
{
    if (fightingNow && !fighting)
    {
        fighting = true;
        before = pins;
        opening = pins;
        return std::nullopt;
    }
    if (!fightingNow && fighting)
    {
        fighting = false;
        Settled settled = Settle(pins, before);
        pins = std::move(before);
        before.clear();
        opening.clear();
        return settled;
    }
    return std::nullopt;
}

namespace
{
void ForgetIn(std::vector<Pin> &pins, const Holdable &thing)
{
    std::erase_if(pins, [&](const Pin &pin) { return SameThing(pin.thing, thing); });
}
} // namespace

void FightBook::Mirror(Layer layer, PinRequest request, const Holdable &thing, Hand hands, bool moving, bool dualWield)
{
    if (!fighting)
        return;
    if (layer == Layer::Normal) [[maybe_unused]]
        const auto displaced = ApplyRequest(before, request, thing, hands, moving, dualWield);
    if (layer == Layer::Normal || layer == Layer::Opening) [[maybe_unused]]
        const auto displaced = ApplyRequest(opening, request, thing, hands, moving, dualWield);
}

void FightBook::Release(Layer layer, Kind kind, Hand hands, BodyPart part)
{
    if (!fighting)
        return;
    if (layer == Layer::Normal) [[maybe_unused]]
        const auto taken = TakeReached(before, kind, hands, part);
    if (layer == Layer::Normal || layer == Layer::Opening) [[maybe_unused]]
        const auto taken = TakeReached(opening, kind, hands, part);
}

void FightBook::Forget(const Holdable &thing)
{
    ForgetIn(before, thing);
    ForgetIn(opening, thing);
}

void IdleBook::Mirror(Layer layer, PinRequest request, const Holdable &thing, Hand hands, bool moving, bool dualWield)
{
    if (layer != Layer::Normal)
        return;
    [[maybe_unused]] const auto displaced = ApplyRequest(normal, request, thing, hands, moving, dualWield);
}

void IdleBook::Release(Layer layer, Kind kind, Hand hands, BodyPart part)
{
    if (layer != Layer::Normal)
        return;
    [[maybe_unused]] const auto taken = TakeReached(normal, kind, hands, part);
}

void IdleBook::Forget(const Holdable &thing)
{
    ForgetIn(normal, thing);
}

const std::vector<Pin> *Layers::Beneath(Layer layer) const noexcept
{
    switch (layer)
    {
    case Layer::Fight:
        return fight.fighting ? &fight.opening : nullptr;
    case Layer::Opening:
        return fight.Remembered();
    case Layer::Idle:
        return &idle.normal;
    case Layer::Normal:
        break;
    }
    return nullptr;
}

std::vector<Displaced> Layers::Apply(std::vector<Pin> &pins, Layer layer, PinRequest request, const Holdable &thing,
                                     Hand hands, bool moving, bool dualWield)
{
    std::vector<Displaced> displaced = ApplyRequest(pins, request, thing, hands, moving, dualWield);
    fight.Mirror(layer, request, thing, hands, moving, dualWield);
    idle.Mirror(layer, request, thing, hands, moving, dualWield);
    return displaced;
}

std::vector<Pin> Layers::Release(std::vector<Pin> &pins, Layer layer, Kind kind, Hand hands, BodyPart part)
{
    std::vector<Pin> released = TakeReached(pins, kind, hands, part);
    fight.Release(layer, kind, hands, part);
    idle.Release(layer, kind, hands, part);
    return released;
}

std::optional<Settled> Layers::Lapse(std::vector<Pin> &pins, bool combatList, const Wants &wants, bool dualWield)
{
    if (combatList != fight.fighting)
        return std::nullopt;
    return ft::Lapse(pins, fight.fighting ? fight.opening : idle.normal, wants, dualWield);
}

void Layers::Adopt(std::vector<Pin> &pins, const Holdable &thing, Hand hands)
{
    AddPin(pins, thing, hands, false);
    AddPin(idle.normal, thing, hands, false);
}

void Layers::Forget(const Holdable &thing)
{
    fight.Forget(thing);
    idle.Forget(thing);
}

PinVerdict JudgePin(const Pin &pin, const PinSeen &seen, bool fighting, bool castInProgress) noexcept
{
    if (!seen.carried)
        return PinVerdict::Drop;
    if (PutBackNow(pin, seen.on, fighting, castInProgress))
        return PinVerdict::PutBack;
    return PinVerdict::Keep;
}

BanVerdict JudgeBan(const BanSeen &seen) noexcept
{
    if (!seen.carried)
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
        switch (JudgeBan(bansSeen[i]))
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
