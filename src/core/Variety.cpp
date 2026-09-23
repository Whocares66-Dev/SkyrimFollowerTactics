#include "core/Variety.h"

#include <algorithm>
#include <cmath>

namespace ft
{

namespace
{
// The Gumbel distribution's mean: the draw's logarithm is centred on it,
// so the factor is 1 in geometric mean and a varied score reads in the
// log as the engine's, give or take, rather than the engine's times 1.78.
constexpr double kEulerGamma = 0.57721566490153286;
} // namespace

Variety::Variety(std::uint64_t seed, VarietySettings settings) : settings_(settings), rng_(seed)
{
}

void Variety::Reset(double /*now*/)
{
    draws_.clear();
    attacks_.clear();
    recent_.clear();
    casts_ = 0;
    lastCast_.clear();
}

// splitmix64: our own, so a test's seed names the same sequence on every
// compiler, which std's distributions do not promise.
double Variety::NextUniform() noexcept
{
    std::uint64_t z = (rng_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    // In (0, 1), both ends open: the Gumbel draw takes a log of a log.
    return (static_cast<double>(z >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

// Has this draw had its turn, or lost it: its spell cast since it was made
// and the minimum hold past, or another enemy. A clock gone backwards is a
// load the caller did not reset for: spent.
bool Variety::Spent(const Draw &draw, std::uint32_t spell, double now, std::uint32_t target) const
{
    const double held = now - draw.at;
    if (held < 0.0 || draw.target != target)
        return true;
    const auto cast = lastCast_.find(spell);
    return cast != lastCast_.end() && cast->second > draw.castsAt && held >= settings_.minHoldSeconds;
}

// Each of the last attack casts that was this spell takes its share: the
// newest the whole penalty, each older one the decay's fraction of the
// one before.
float Variety::Recency(std::uint32_t spell) const noexcept
{
    double factor = 1.0;
    double penalty = settings_.recentPenalty;
    for (const std::uint32_t cast : recent_)
    {
        if (cast == spell)
            factor *= 1.0 - std::clamp(penalty, 0.0, 1.0);
        penalty *= settings_.recencyDecay;
    }
    return static_cast<float>(factor);
}

Variety::Varied Variety::Adjust(std::uint64_t entry, std::uint32_t spell, float score, double now, std::uint32_t target)
{
    Varied out;
    out.score = score;
    if (!(score > 0.0f))
    {
        Release(entry);
        return out;
    }
    attacks_.insert(spell);
    out.recency = Recency(spell);

    auto draw = draws_.find(entry);
    if (draw == draws_.end() || Spent(draw->second, spell, now, target))
    {
        const double gumbel = -std::log(-std::log(NextUniform()));
        const double bound = std::log(std::max(1.0, settings_.maxDrawFactor));
        const double factor = std::exp(std::clamp(settings_.temperature * (gumbel - kEulerGamma), -bound, bound));
        draw = draws_.insert_or_assign(entry, Draw{static_cast<float>(factor), now, ++drawn_, casts_, target}).first;
        out.fresh = true;
    }
    out.draw = draw->second.factor;
    out.drawn = draw->second.number;
    out.score = score * out.recency * out.draw;
    return out;
}

void Variety::Release(std::uint64_t entry)
{
    draws_.erase(entry);
}

void Variety::NoteCast(std::uint32_t spell)
{
    if (!attacks_.contains(spell))
        return;
    lastCast_[spell] = ++casts_;
    recent_.insert(recent_.begin(), spell);
    if (recent_.size() > settings_.recencyMemory)
        recent_.resize(settings_.recencyMemory);
}

} // namespace ft
