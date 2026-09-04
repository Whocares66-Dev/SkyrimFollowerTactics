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

    case ActionKind::EquipWeapon:
    case ActionKind::EquipSpell:
    case ActionKind::EquipArrows:
    case ActionKind::EquipArmor:
        // A pin is cheap and its result is visible at once, so this only
        // needs to be long enough not to thrash. Re-pinning what is already
        // pinned is prevented by availability, not by this.
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

bool IsEquip(ActionKind action) noexcept
{
    return KindOf(action) != Kind::Other;
}

Kind KindOf(ActionKind action) noexcept
{
    switch (action)
    {
    case ActionKind::EquipWeapon:
        return Kind::Weapon;
    case ActionKind::EquipSpell:
        return Kind::Spell;
    case ActionKind::EquipArrows:
        return Kind::Ammo;
    case ActionKind::EquipArmor:
        return Kind::Armor;
    default:
        return Kind::Other;
    }
}

PredicateKind AboveOf(PredicateKind predicate) noexcept
{
    switch (predicate)
    {
    case PredicateKind::HealthPctBelow:
        return PredicateKind::HealthPctAbove;
    case PredicateKind::StaminaPctBelow:
        return PredicateKind::StaminaPctAbove;
    case PredicateKind::MagickaPctBelow:
        return PredicateKind::MagickaPctAbove;
    default:
        return predicate;
    }
}

bool IsAbove(PredicateKind predicate) noexcept
{
    return predicate == PredicateKind::HealthPctAbove || predicate == PredicateKind::StaminaPctAbove ||
           predicate == PredicateKind::MagickaPctAbove;
}

Extremes ExtremesOf(PredicateKind predicate) noexcept
{
    switch (predicate)
    {
    case PredicateKind::HealthPctBelow:
        return {PredicateKind::HealthLowest, PredicateKind::HealthHighest};
    case PredicateKind::Armor:
        return {PredicateKind::ArmorLowest, PredicateKind::ArmorHighest};
    default:
        return {predicate, predicate};
    }
}

bool IsExtreme(PredicateKind predicate) noexcept
{
    return predicate == PredicateKind::HealthLowest || predicate == PredicateKind::HealthHighest ||
           predicate == PredicateKind::ArmorLowest || predicate == PredicateKind::ArmorHighest;
}

bool IsPredicateValidFor(SubjectKind subject, PredicateKind predicate) noexcept
{
    // The matrix is driven by what Snapshot actually carries. When a sensor is
    // added -- ally magicka, say -- this table is the one place to widen, and
    // the UI menu widens with it for free.
    //
    // An above predicate is answerable exactly where its below counterpart
    // is: the same number, the other side.
    if (IsAbove(predicate))
    {
        for (std::size_t i = 0; i < static_cast<std::size_t>(PredicateKind::COUNT); ++i)
        {
            const auto below = static_cast<PredicateKind>(i);
            if (below != predicate && AboveOf(below) == predicate)
                return IsPredicateValidFor(subject, below);
        }
        return false;
    }
    // A status and the armour are read off every actor the snapshot
    // carries, so they are answerable about any of them. The extremes are
    // of a group.
    if (predicate == PredicateKind::Status || predicate == PredicateKind::Armor ||
        predicate == PredicateKind::Resistance)
        return true;
    if (IsExtreme(predicate))
        return subject == SubjectKind::Ally || subject == SubjectKind::Enemy;
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
        case PredicateKind::CombatBegins:
        case PredicateKind::CombatEnds:
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
