#include "core/Spells.h"

namespace ft
{

SpellsKnown ClassifySpells(std::span<const SpellSeen> seen)
{
    SpellsKnown out;
    for (const SpellSeen &s : seen)
    {
        switch (s.kind)
        {
        case SpellSeen::Kind::Shout:
            if (!s.wrapper && s.highestWord >= 0)
                out.known.push_back(s.id);
            break;
        case SpellSeen::Kind::Scroll:
            if (s.carried > 0)
                out.known.push_back(s.id);
            break;
        case SpellSeen::Kind::Staff:
            if (s.carried > 0)
            {
                out.known.push_back(s.id);
                if (!s.charged)
                    out.spent.push_back(s.id);
            }
            break;
        case SpellSeen::Kind::Power:
            out.known.push_back(s.id);
            if (s.greater && s.usedToday)
                out.usedToday.push_back(s.id);
            break;
        case SpellSeen::Kind::Spell:
            if (s.castable)
            {
                out.known.push_back(s.id);
                out.castable.push_back(s.id);
            }
            break;
        }
    }
    return out;
}

std::optional<std::size_t> StaffCopyToCast(std::span<const StaffCopy> copies, float cost) noexcept
{
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < copies.size(); ++i)
    {
        if (copies[i].charge < cost)
            continue;
        if (copies[i].inHand)
            return i;
        if (!best || copies[i].charge > copies[*best].charge)
            best = i;
    }
    return best;
}

float RemainingOn(std::span<const EffectSeen> effects, std::span<const std::uint32_t> sources) noexcept
{
    float best = 0.0f;
    for (const EffectSeen &effect : effects)
    {
        if (effect.spell == 0 || effect.duration <= 0.0f)
            continue;
        bool ours = false;
        for (const std::uint32_t source : sources)
            ours = ours || source == effect.spell;
        if (!ours)
            continue;
        const float left = effect.duration - effect.elapsed;
        if (left > best)
            best = left;
    }
    return best;
}

} // namespace ft
