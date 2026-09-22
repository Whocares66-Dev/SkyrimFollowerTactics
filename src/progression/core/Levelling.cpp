#include "progression/core/Levelling.h"

#include <cmath>

namespace fp
{

double SkillXp(const SkillUsage &u, double points) noexcept
{
    return points * u.useMult + u.useOffset;
}

double SkillThreshold(const Rules &r, const SkillUsage &u, int level) noexcept
{
    if (level >= r.skillCap)
        return 0.0;
    return std::pow(static_cast<double>(level), r.skillUseCurve) * u.improveMult + u.improveOffset;
}

double XpForSkillLevel(const Rules &r, int level) noexcept
{
    return level * r.xpPerSkillRank;
}

double LevelThreshold(const Rules &r, int level) noexcept
{
    return r.levelUpBase + r.levelUpMult * level;
}

double XpToReach(const Rules &r, int level) noexcept
{
    double xp = 0.0;
    for (int l = 1; l < level; ++l)
        xp += LevelThreshold(r, l);
    return xp;
}

int LevelFor(const Rules &r, double xp) noexcept
{
    int level = 1;
    // Past a few hundred levels the numbers stop meaning anything; the
    // bound only keeps nonsense settings from spinning here.
    while (level < 1000 && xp >= LevelThreshold(r, level))
    {
        xp -= LevelThreshold(r, level);
        ++level;
    }
    return level;
}

} // namespace fp
