#include "Rule.h"

namespace ft
{

double MinimumCooldown(ActionKind action) noexcept
{
    // The consumables, the poisons and the soul gems: the measured
    // queue-to-effect latency is about two seconds, plus a margin so the
    // next evaluation sees the result of this one. Deliberately not longer:
    // one potion is often not enough, and a follower who is still badly
    // hurt should drink again promptly. Food and ingredients take the
    // potion's number until one of their own is measured; they go through
    // the same equip call, as a poison and a gem go through the engine's
    // own routines.
    if (IsConsume(action) || IsApply(action) || IsCharge(action))
        return 3.0;
    // The casts. Measured: the AI picks the package up on the same tick,
    // and a heal lands 0.9-2.2 s later. Two seconds lets the next
    // evaluation see the result of this one without re-firing into a cast
    // still in progress; the cast's lease covers the case where it
    // has not landed. A power or a shout rides a package of its own and
    // takes the same number; the shout's own recovery time is the engine's.
    if (IsCast(action))
        return 2.0;
    // A pin is cheap and its result is visible at once, so this only needs
    // to be long enough not to thrash. Re-pinning what is already pinned is
    // prevented by availability, not by this.
    if (IsEquip(action))
        return 1.0;
    switch (action)
    {
    case ActionKind::PowerAttack:
    case ActionKind::PowerBash:
        // After the swing: the lease or the request covers the swing itself,
        // and this is the recovery before the next.
        return 1.5;
    case ActionKind::Bash:
        return 1.0;
    case ActionKind::Attack:
        // Long enough that a follower is not flicked between two enemies on
        // consecutive ticks; the key is the action alone, not the target, so
        // one Target blocks every other for this long. Raise it if two
        // seconds still looks like dithering in play.
        return 2.0;
    default:
        return 0.0;
    }
}

bool IsEquip(ActionKind action) noexcept
{
    return KindOf(action) != Kind::Other;
}

bool TakesHand(ActionKind action) noexcept
{
    return action == ActionKind::EquipWeapon || action == ActionKind::EquipSpell;
}

bool IsConsume(ActionKind action) noexcept
{
    switch (action)
    {
    case ActionKind::DrinkStrongest:
    case ActionKind::DrinkWeakest:
    case ActionKind::DrinkAny:
    case ActionKind::DrinkPotion:
    case ActionKind::EatStrongestFood:
    case ActionKind::EatWeakestFood:
    case ActionKind::EatFood:
    case ActionKind::EatStrongestIngredient:
    case ActionKind::EatWeakestIngredient:
    case ActionKind::EatIngredient:
        return true;
    default:
        return false;
    }
}

bool IsPolicy(ActionKind action) noexcept
{
    switch (action)
    {
    case ActionKind::DrinkStrongest:
    case ActionKind::DrinkWeakest:
    case ActionKind::EatStrongestFood:
    case ActionKind::EatWeakestFood:
    case ActionKind::EatStrongestIngredient:
    case ActionKind::EatWeakestIngredient:
    case ActionKind::ApplyStrongest:
    case ActionKind::ApplyWeakest:
        return true;
    default:
        return false;
    }
}

bool IsStrongest(ActionKind action) noexcept
{
    return action == ActionKind::DrinkStrongest || action == ActionKind::EatStrongestFood ||
           action == ActionKind::EatStrongestIngredient || action == ActionKind::ApplyStrongest;
}

bool IsAny(ActionKind action) noexcept
{
    return action == ActionKind::ApplyAny || action == ActionKind::DrinkAny;
}

bool ChoosesForm(ActionKind action) noexcept
{
    return IsPolicy(action) || IsAny(action) || IsArrowsPolicy(action) ||
           action == ActionKind::ChargeStrongestSoulGem || action == ActionKind::ChargeWeakestSoulGem;
}

bool IsApply(ActionKind action) noexcept
{
    return action == ActionKind::ApplyStrongest || action == ActionKind::ApplyWeakest ||
           action == ActionKind::ApplyAny || action == ActionKind::ApplyPoison;
}

bool IsCharge(ActionKind action) noexcept
{
    return action == ActionKind::ChargeStrongestSoulGem || action == ActionKind::ChargeWeakestSoulGem ||
           action == ActionKind::ChargeSoulGem;
}

ConsumableKind ConsumableOf(ActionKind action) noexcept
{
    switch (action)
    {
    case ActionKind::EatStrongestFood:
    case ActionKind::EatWeakestFood:
    case ActionKind::EatFood:
        return ConsumableKind::Food;
    case ActionKind::EatStrongestIngredient:
    case ActionKind::EatWeakestIngredient:
    case ActionKind::EatIngredient:
        return ConsumableKind::Ingredient;
    default:
        return IsApply(action)    ? ConsumableKind::Poison
               : IsCharge(action) ? ConsumableKind::SoulGem
                                  : ConsumableKind::Potion;
    }
}

bool IsCast(ActionKind action) noexcept
{
    return action == ActionKind::CastSpell || action == ActionKind::UsePower || action == ActionKind::Shout ||
           action == ActionKind::UseScroll;
}

bool IsBlow(ActionKind action) noexcept
{
    return action == ActionKind::PowerAttack || action == ActionKind::Bash || action == ActionKind::PowerBash;
}

bool NamesConsumable(ActionKind action) noexcept
{
    switch (action)
    {
    case ActionKind::DrinkPotion:
    case ActionKind::EatFood:
    case ActionKind::EatIngredient:
    case ActionKind::ApplyPoison:
    case ActionKind::ChargeSoulGem:
        return true;
    default:
        return false;
    }
}

bool NamesForm(ActionKind action) noexcept
{
    return IsCast(action) || (IsEquip(action) && !IsArrowsPolicy(action)) || NamesConsumable(action);
}

bool IsArrowsPolicy(ActionKind action) noexcept
{
    return action == ActionKind::EquipStrongestArrows || action == ActionKind::EquipWeakestArrows;
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
    case ActionKind::EquipStrongestArrows:
    case ActionKind::EquipWeakestArrows:
        return Kind::Ammo;
    case ActionKind::EquipArmor:
        return Kind::Armor;
    default:
        return Kind::Other;
    }
}

bool IsActionTargetValidFor(SubjectKind subject, ActionTargetKind target) noexcept
{
    // "Enemy" reads from the condition: under an enemy condition it is the
    // one the condition matched; under anyone else, whoever the follower
    // is fighting, else the nearest. "Attacker" is whoever last hit the
    // actor the condition bound. Under an enemy or a corpse condition that
    // is one of us, and no rule means to aim at that, so the pair is not
    // offered and not answered; ResolveActionTarget has no branch for it.
    switch (target)
    {
    case ActionTargetKind::Ally:
        return subject == SubjectKind::Ally || subject == SubjectKind::Follower;
    case ActionTargetKind::Enemy:
        return subject != SubjectKind::Corpse;
    case ActionTargetKind::Attacker:
        return subject != SubjectKind::Enemy && subject != SubjectKind::Corpse;
    case ActionTargetKind::Corpse:
        return subject == SubjectKind::Corpse;
    default:
        return true;
    }
}

bool IsActionValidFor(ActionTargetKind target, ActionKind action) noexcept
{
    switch (action)
    {
    case ActionKind::None:
    case ActionKind::CastSpell:
        return true;
    case ActionKind::UsePower:
    case ActionKind::Shout:
    case ActionKind::UseScroll:
        // Aimed anywhere but at a corpse: a Reanimate is a spell.
        return target != ActionTargetKind::Corpse;
    case ActionKind::Attack:
    case ActionKind::PowerAttack:
    case ActionKind::Bash:
    case ActionKind::PowerBash:
        return target == ActionTargetKind::Enemy || target == ActionTargetKind::Attacker;
    default:
        // Potions, pins, and what the follower does with their own feet.
        return target == ActionTargetKind::Self;
    }
}

bool IsStatusValidFor(SubjectKind subject, StatusKind status) noexcept
{
    switch (subject)
    {
    case SubjectKind::Self:
        // No action can be taken while any of these holds.
        return status != StatusKind::BleedingOut && status != StatusKind::Casting && status != StatusKind::Fleeing &&
               status != StatusKind::Staggered;
    case SubjectKind::Player:
        // The player neither bleeds out nor flees.
        return status != StatusKind::BleedingOut && status != StatusKind::Fleeing;
    default:
        return true;
    }
}

bool IsDamageKindValidFor(PredicateKind predicate, DamageKind kind) noexcept
{
    if (predicate == PredicateKind::HitType)
        return kind != DamageKind::Any; // every actor hits with something
    if (IsResistance(predicate))
        return kind != DamageKind::Melee && kind != DamageKind::Ranged && kind != DamageKind::Any;
    return true;
}

void Reconcile(Rule &rule) noexcept
{
    if (!IsActionTargetValidFor(rule.subject, rule.actionTarget))
    {
        rule.actionTarget = ActionTargetKind::Self;
        rule.actionTargetForm = 0;
    }
    for (Action &a : rule.actions)
    {
        if (!IsActionValidFor(rule.actionTarget, a.kind))
            a = {};
    }
}

Grid GridOf(PredicateKind predicate) noexcept
{
    using M = Measure;
    using S = Side;
    switch (predicate)
    {
    case PredicateKind::HealthPctBelow:
        return {M::Health, S::Below};
    case PredicateKind::HealthPctAbove:
        return {M::Health, S::Above};
    case PredicateKind::HealthLowest:
        return {M::Health, S::Lowest};
    case PredicateKind::HealthHighest:
        return {M::Health, S::Highest};
    case PredicateKind::StaminaPctBelow:
        return {M::Stamina, S::Below};
    case PredicateKind::StaminaPctAbove:
        return {M::Stamina, S::Above};
    case PredicateKind::StaminaLowest:
        return {M::Stamina, S::Lowest};
    case PredicateKind::StaminaHighest:
        return {M::Stamina, S::Highest};
    case PredicateKind::MagickaPctBelow:
        return {M::Magicka, S::Below};
    case PredicateKind::MagickaPctAbove:
        return {M::Magicka, S::Above};
    case PredicateKind::MagickaLowest:
        return {M::Magicka, S::Lowest};
    case PredicateKind::MagickaHighest:
        return {M::Magicka, S::Highest};
    case PredicateKind::ArmorPctBelow:
        return {M::Armor, S::Below};
    case PredicateKind::ArmorPctAbove:
        return {M::Armor, S::Above};
    case PredicateKind::ArmorLowest:
        return {M::Armor, S::Lowest};
    case PredicateKind::ArmorHighest:
        return {M::Armor, S::Highest};
    case PredicateKind::ResistancePctBelow:
        return {M::Resistance, S::Below};
    case PredicateKind::ResistancePctAbove:
        return {M::Resistance, S::Above};
    case PredicateKind::ResistanceLowest:
        return {M::Resistance, S::Lowest};
    case PredicateKind::ResistanceHighest:
        return {M::Resistance, S::Highest};
    default:
        return {};
    }
}

PredicateKind PredicateAt(Measure measure, Side side) noexcept
{
    if (measure == Measure::None || side == Side::None)
        return PredicateKind::Any;
    for (std::size_t i = 0; i < static_cast<std::size_t>(PredicateKind::COUNT); ++i)
    {
        const auto p = static_cast<PredicateKind>(i);
        const Grid g = GridOf(p);
        if (g.measure == measure && g.side == side)
            return p;
    }
    return PredicateKind::Any;
}

PredicateKind AboveOf(PredicateKind predicate) noexcept
{
    const Grid g = GridOf(predicate);
    return g.side == Side::Below ? PredicateAt(g.measure, Side::Above) : predicate;
}

PredicateKind BelowOf(PredicateKind predicate) noexcept
{
    const Grid g = GridOf(predicate);
    return g.side == Side::Above ? PredicateAt(g.measure, Side::Below) : predicate;
}

bool IsAbove(PredicateKind predicate) noexcept
{
    return GridOf(predicate).side == Side::Above;
}

Extremes ExtremesOf(PredicateKind predicate) noexcept
{
    const Grid g = GridOf(predicate);
    if (g.side != Side::Below)
        return {predicate, predicate};
    return {PredicateAt(g.measure, Side::Lowest), PredicateAt(g.measure, Side::Highest)};
}

bool IsExtreme(PredicateKind predicate) noexcept
{
    const Side side = GridOf(predicate).side;
    return side == Side::Lowest || side == Side::Highest;
}

bool IsResistance(PredicateKind predicate) noexcept
{
    return GridOf(predicate).measure == Measure::Resistance;
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
        return IsPredicateValidFor(subject, BelowOf(predicate));
    // The corpses answer their own three questions and no other; nobody
    // else answers them.
    const bool corpseQuestion = predicate == PredicateKind::CorpseNone || predicate == PredicateKind::LevelHighest ||
                                predicate == PredicateKind::LevelLowest;
    if (subject == SubjectKind::Corpse || corpseQuestion)
        return subject == SubjectKind::Corpse && corpseQuestion;
    // The kind of being is a question about a group: an enemy may be
    // anything, an ally anyone. The follower, the player and a named
    // follower are each one being, and a rule about what they are would be
    // true always or never.
    if (predicate == PredicateKind::Type)
        return subject == SubjectKind::Ally || subject == SubjectKind::Enemy;
    // A status, the armour, the resistances, the hands, the hits and the
    // summons are read off every actor the snapshot carries, so they are
    // answerable about any of them. The extremes are of a group.
    if (predicate == PredicateKind::Status || predicate == PredicateKind::ArmorPctBelow ||
        predicate == PredicateKind::ResistancePctBelow || predicate == PredicateKind::HitType ||
        predicate == PredicateKind::HitBy || predicate == PredicateKind::SummonNone ||
        predicate == PredicateKind::SummonActive)
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
        case PredicateKind::CombatBegins:
        case PredicateKind::CombatEnds:
        case PredicateKind::WeaponChargeNeeded:
        case PredicateKind::WeaponPoisonNone:
        case PredicateKind::WeaponPoisonActive:
            return true;
        default:
            return false;
        }

    case SubjectKind::Player:
    case SubjectKind::Ally:
        // Any: always true, and worth having so a rule can aim at the
        // player or an ally under the heading a reader looks for it.
        // No ally count: how many allies there are changes too rarely
        // to be a condition.
        switch (predicate)
        {
        case PredicateKind::Any:
        case PredicateKind::HealthPctBelow:
        case PredicateKind::MagickaPctBelow:
        case PredicateKind::StaminaPctBelow:
            return true;
        default:
            return false;
        }

    case SubjectKind::Follower:
        // One ally, asked about alone: everything an ally answers -- and
        // Any, which for a NAMED follower is "they are with us", and so
        // worth asking.
        return predicate == PredicateKind::Any || IsPredicateValidFor(SubjectKind::Ally, predicate);

    case SubjectKind::Enemy:
        switch (predicate)
        {
        case PredicateKind::Any:
        case PredicateKind::HealthPctBelow:
        case PredicateKind::MagickaPctBelow:
        case PredicateKind::StaminaPctBelow:
        case PredicateKind::Attacking:
        case PredicateKind::AttackedBy:
            return true;
        default:
            return false;
        }

    default:
        return false;
    }
}

} // namespace ft
