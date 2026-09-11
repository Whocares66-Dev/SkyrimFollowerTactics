#include "HitTable.h"

namespace ft
{

void HitTable::Note(ActorId target, DamageKind kind, ActorId attacker, double when)
{
    auto &entry = hits_[target][static_cast<std::size_t>(kind)];
    entry.when = when;
    entry.attacker = attacker;
}

Attacked HitTable::Lately(ActorId target, double now, double window) const
{
    Attacked out;
    const auto it = hits_.find(target);
    if (it == hits_.end())
        return out;
    double latest = -1.0;
    for (std::size_t k = 0; k < it->second.size(); ++k)
    {
        const Entry &entry = it->second[k];
        if (entry.when < 0.0 || now - entry.when > window)
            continue;
        out.kinds |= Bit(static_cast<DamageKind>(k));
        if (entry.when > latest)
        {
            latest = entry.when;
            out.attacker = entry.attacker;
        }
    }
    return out;
}

} // namespace ft
