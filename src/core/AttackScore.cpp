#include "core/AttackScore.h"

#include <algorithm>

namespace ft
{

namespace
{
// No cycle is shorter than the engine's own floor on a hold.
constexpr double kMinCycleSeconds = 0.1;
} // namespace

double MagickaPrice(const MagickaPool &pool) noexcept
{
    if (!(pool.max > 0.0f))
        return 0.0;
    const double drained = 1.0 - std::clamp(static_cast<double>(pool.current) / pool.max, 0.0, 1.0);
    const double perPoint =
        pool.regenPerSecond > 0.0f ? std::min(1.0 / pool.regenPerSecond, kMaxSecondsPerPoint) : kMaxSecondsPerPoint;
    return drained * drained * perPoint;
}

double ScrollReserve(const MagickaPool &pool) noexcept
{
    if (!(pool.max > 0.0f))
        return 1.0;
    const double drained = 1.0 - std::clamp(static_cast<double>(pool.current) / pool.max, 0.0, 1.0);
    return drained * drained;
}

double HoldSeconds(double offensiveMult) noexcept
{
    const double t = std::clamp(offensiveMult, 0.0, 1.0);
    return std::max(0.1, 0.5 + t * (1.5 - 0.5));
}

SpellCycle ReleasedCycle(double chargeSeconds, double holdSeconds, double cost) noexcept
{
    return {std::max(kMinCycleSeconds, std::max(0.0, chargeSeconds) + std::max(0.0, holdSeconds)), std::max(0.0, cost)};
}

SpellCycle StreamCycle(double scoringSeconds, double costPerSecond) noexcept
{
    const double seconds = std::max(kMinCycleSeconds, scoringSeconds);
    return {seconds, std::max(0.0, costPerSecond) * seconds};
}

double PerSecond(double damagePerCycle, const SpellCycle &cycle, double price) noexcept
{
    const double time = std::max(kMinCycleSeconds, cycle.seconds) + std::max(0.0, price) * cycle.magicka;
    return damagePerCycle / time;
}

} // namespace ft
