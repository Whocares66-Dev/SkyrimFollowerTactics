#include "Rule.h"

namespace ft
{

double MinimumCooldown(ActionKind action) noexcept
{
    switch (action)
    {
    case ActionKind::DrinkHealthPotion:
    case ActionKind::DrinkMagickaPotion:
    case ActionKind::DrinkStaminaPotion:
    case ActionKind::DrinkPotion:
        // The measured queue-to-effect latency is about two seconds, plus a
        // margin so the next evaluation sees the result of this one.
        // Deliberately not longer: one potion is often not enough, and a
        // follower who is still badly hurt should drink again promptly.
        return 3.0;

    case ActionKind::CastSpell:
        // Measured: the AI picks the package up on the same tick, and a heal
        // lands 0.9-2.2 s later. Two seconds lets the next evaluation see the
        // result of this one without re-firing into a cast still in progress;
        // the package pool's lease covers the case where it has not landed.
        return 2.0;

    case ActionKind::EquipSpell:
        // Swapping what is in hand is cheap and its result is visible at once,
        // so this only needs to be long enough not to thrash. Re-equipping what
        // is already equipped is prevented by availability, not by this.
        return 1.0;

    case ActionKind::StopCombat:
        // Changes whether the follower is in combat, which conditions read.
        return 1.0;

    case ActionKind::Flee:
    case ActionKind::HoldPosition:
        // Movement takes time to change any distance a condition reads, and
        // re-pushing a package every tick would give it no chance to run.
        return 1.0;

    default:
        return 0.0;
    }
}

bool IsPredicateValidFor(SubjectKind subject, PredicateKind predicate) noexcept
{
    // The matrix is driven by what Snapshot actually carries. When a sensor is
    // added -- ally magicka, say -- this table is the one place to widen, and
    // the UI menu widens with it for free.
    switch (subject)
    {
    case SubjectKind::Self:
        switch (predicate)
        {
        case PredicateKind::Any:
        case PredicateKind::HealthPctBelow:
        case PredicateKind::MagickaPctBelow:
        case PredicateKind::StaminaPctBelow:
        case PredicateKind::InBleedout:
        case PredicateKind::InCombat:
            return true;
        default:
            // WithinDistance is meaningless (distance to what?) and CountAtLeast
            // needs a group.
            return false;
        }

    case SubjectKind::Player:
        switch (predicate)
        {
        case PredicateKind::Any:
        case PredicateKind::HealthPctBelow:
        case PredicateKind::InCombat:
        case PredicateKind::WithinDistance:
            return true;
        default:
            return false;
        }

    case SubjectKind::Ally:
        switch (predicate)
        {
        case PredicateKind::Any:
        case PredicateKind::HealthPctBelow:
        case PredicateKind::InBleedout:
        case PredicateKind::WithinDistance:
        case PredicateKind::CountAtLeast:
            return true;
        default:
            return false;
        }

    case SubjectKind::Enemy:
        switch (predicate)
        {
        case PredicateKind::Any:
        case PredicateKind::HealthPctBelow:
        case PredicateKind::WithinDistance:
        case PredicateKind::CountAtLeast:
            return true;
        default:
            return false;
        }

    case SubjectKind::CurrentTarget:
        switch (predicate)
        {
        case PredicateKind::Any:
        case PredicateKind::HealthPctBelow:
        case PredicateKind::WithinDistance:
            return true;
        default:
            return false;
        }

    default:
        return false;
    }
}

} // namespace ft
