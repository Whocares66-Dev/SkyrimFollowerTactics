#include "core/HitKinds.h"

namespace ft
{

DamageKind KindOfResist(Resist resist) noexcept
{
    switch (resist)
    {
    case Resist::Fire:
        return DamageKind::Fire;
    case Resist::Frost:
        return DamageKind::Frost;
    case Resist::Shock:
        return DamageKind::Shock;
    case Resist::Poison:
        return DamageKind::Poison;
    case Resist::Other:
    default:
        return DamageKind::Magic;
    }
}

std::vector<DamageKind> KindsOfHit(const HitSeen &hit)
{
    std::vector<DamageKind> kinds;
    if (!hit.magic)
    {
        kinds.push_back(hit.projectile ? DamageKind::Ranged : DamageKind::Melee);
        return kinds;
    }
    for (const Resist resist : hit.detrimental)
    {
        kinds.push_back(DamageKind::Magic);
        kinds.push_back(KindOfResist(resist));
    }
    if (hit.poison)
        kinds.push_back(DamageKind::Poison);
    return kinds;
}

} // namespace ft
