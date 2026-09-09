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
    case ActionKind::DrinkWeakestHealthPotion:
    case ActionKind::DrinkWeakestMagickaPotion:
    case ActionKind::DrinkWeakestStaminaPotion:
    case ActionKind::DrinkPotion:
    case ActionKind::EatFood:
    case ActionKind::EatIngredient:
        // The measured queue-to-effect latency is about two seconds, plus a
        // margin so the next evaluation sees the result of this one.
        // Deliberately not longer: one potion is often not enough, and a
        // follower who is still badly hurt should drink again promptly.
        // Food and ingredients take the potion's number until one of their
        // own is measured; they go through the same equip call.
        return 3.0;

    case ActionKind::CastSpell:
    case ActionKind::UsePower:
    case ActionKind::Shout:
        // Measured: the AI picks the package up on the same tick, and a heal
        // lands 0.9-2.2 s later. Two seconds lets the next evaluation see the
        // result of this one without re-firing into a cast still in progress;
        // the package pool's lease covers the case where it has not landed.
        // A power or a shout rides a package of its own and takes the same
        // number; the shout's own recovery time is the engine's.
        return 2.0;

    case ActionKind::EquipWeapon:
    case ActionKind::EquipSpell:
    case ActionKind::EquipArrows:
    case ActionKind::EquipArmor:
        // A pin is cheap and its result is visible at once, so this only
        // needs to be long enough not to thrash. Re-pinning what is already
        // pinned is prevented by availability, not by this.
        return 1.0;

    case ActionKind::Target:
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

bool IsConsume(ActionKind action) noexcept
{
    switch (action)
    {
    case ActionKind::DrinkHealthPotion:
    case ActionKind::DrinkMagickaPotion:
    case ActionKind::DrinkStaminaPotion:
    case ActionKind::DrinkWeakestHealthPotion:
    case ActionKind::DrinkWeakestMagickaPotion:
    case ActionKind::DrinkWeakestStaminaPotion:
    case ActionKind::DrinkPotion:
    case ActionKind::EatFood:
    case ActionKind::EatIngredient:
        return true;
    default:
        return false;
    }
}

ConsumableKind ConsumableOf(ActionKind action) noexcept
{
    switch (action)
    {
    case ActionKind::EatFood:
        return ConsumableKind::Food;
    case ActionKind::EatIngredient:
        return ConsumableKind::Ingredient;
    default:
        return ConsumableKind::Potion;
    }
}

bool IsCast(ActionKind action) noexcept
{
    return action == ActionKind::CastSpell || action == ActionKind::UsePower || action == ActionKind::Shout;
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

bool IsActionTargetValidFor(SubjectKind subject, ActionTargetKind target) noexcept
{
    switch (target)
    {
    case ActionTargetKind::Ally:
        return subject == SubjectKind::Ally || subject == SubjectKind::Follower;
    case ActionTargetKind::Enemy:
        return subject == SubjectKind::Enemy || subject == SubjectKind::CurrentTarget;
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
        return true;
    case ActionKind::CastSpell:
        return true;
    case ActionKind::UsePower:
    case ActionKind::Shout:
        // Aimed anywhere but at a corpse: a Reanimate is a spell.
        return target != ActionTargetKind::Corpse;
    case ActionKind::Target:
        return target == ActionTargetKind::Enemy || target == ActionTargetKind::Attacker;
    default:
        // Potions, pins, and what the follower does with their own feet.
        return target == ActionTargetKind::Self;
    }
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
    case PredicateKind::ArmorPctBelow:
        return PredicateKind::ArmorPctAbove;
    case PredicateKind::ResistancePctBelow:
        return PredicateKind::ResistancePctAbove;
    default:
        return predicate;
    }
}

bool IsAbove(PredicateKind predicate) noexcept
{
    switch (predicate)
    {
    case PredicateKind::HealthPctAbove:
    case PredicateKind::StaminaPctAbove:
    case PredicateKind::MagickaPctAbove:
    case PredicateKind::ArmorPctAbove:
    case PredicateKind::ResistancePctAbove:
        return true;
    default:
        return false;
    }
}

Extremes ExtremesOf(PredicateKind predicate) noexcept
{
    switch (predicate)
    {
    case PredicateKind::HealthPctBelow:
        return {PredicateKind::HealthLowest, PredicateKind::HealthHighest};
    case PredicateKind::StaminaPctBelow:
        return {PredicateKind::StaminaLowest, PredicateKind::StaminaHighest};
    case PredicateKind::MagickaPctBelow:
        return {PredicateKind::MagickaLowest, PredicateKind::MagickaHighest};
    case PredicateKind::ArmorPctBelow:
        return {PredicateKind::ArmorLowest, PredicateKind::ArmorHighest};
    case PredicateKind::ResistancePctBelow:
        return {PredicateKind::ResistanceLowest, PredicateKind::ResistanceHighest};
    default:
        return {predicate, predicate};
    }
}

bool IsExtreme(PredicateKind predicate) noexcept
{
    switch (predicate)
    {
    case PredicateKind::HealthLowest:
    case PredicateKind::HealthHighest:
    case PredicateKind::StaminaLowest:
    case PredicateKind::StaminaHighest:
    case PredicateKind::MagickaLowest:
    case PredicateKind::MagickaHighest:
    case PredicateKind::ArmorLowest:
    case PredicateKind::ArmorHighest:
    case PredicateKind::ResistanceLowest:
    case PredicateKind::ResistanceHighest:
        return true;
    default:
        return false;
    }
}

bool IsResistance(PredicateKind predicate) noexcept
{
    return predicate == PredicateKind::ResistancePctBelow || predicate == PredicateKind::ResistancePctAbove ||
           predicate == PredicateKind::ResistanceLowest || predicate == PredicateKind::ResistanceHighest;
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
    // The corpses answer their own three questions and no other; nobody
    // else answers them.
    const bool corpseQuestion = predicate == PredicateKind::CorpseNone || predicate == PredicateKind::LevelHighest ||
                                predicate == PredicateKind::LevelLowest;
    if (subject == SubjectKind::Corpse || corpseQuestion)
        return subject == SubjectKind::Corpse && corpseQuestion;
    // A status, the armour, the resistances, the hits and the summons are
    // read off every actor the snapshot carries, so they are answerable
    // about any of them. The extremes are of a group.
    if (predicate == PredicateKind::Status || predicate == PredicateKind::ArmorPctBelow ||
        predicate == PredicateKind::ResistancePctBelow || predicate == PredicateKind::AttackedBy ||
        predicate == PredicateKind::SummonNone || predicate == PredicateKind::SummonActive)
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
            return true;
        default:
            // CountAtLeast needs a group.
            return false;
        }

    case SubjectKind::Player:
        // Not Any: the player is always there, so "Player: Any" is "Self:
        // Any" under another name, and the menu should not offer it twice.
        switch (predicate)
        {
        case PredicateKind::HealthPctBelow:
        case PredicateKind::MagickaPctBelow:
        case PredicateKind::StaminaPctBelow:
            return true;
        default:
            return false;
        }

    case SubjectKind::Ally:
        // Not Any: a follower always has an ally, the player, so it would
        // always be true -- that is Self: Any.
        switch (predicate)
        {
        case PredicateKind::HealthPctBelow:
        case PredicateKind::MagickaPctBelow:
        case PredicateKind::StaminaPctBelow:
        case PredicateKind::CountAtLeast:
            return true;
        default:
            return false;
        }

    case SubjectKind::Follower:
        // One ally, asked about alone: everything an ally answers but the
        // count, which is a group's -- and Any, which for a NAMED follower
        // is "they are with us", and so worth asking.
        return predicate == PredicateKind::Any ||
               (predicate != PredicateKind::CountAtLeast && IsPredicateValidFor(SubjectKind::Ally, predicate));

    case SubjectKind::Enemy:
        switch (predicate)
        {
        case PredicateKind::Any:
        case PredicateKind::HealthPctBelow:
        case PredicateKind::MagickaPctBelow:
        case PredicateKind::StaminaPctBelow:
        case PredicateKind::CountAtLeast:
        case PredicateKind::AttackingPlayer:
        case PredicateKind::TargetOfPlayer:
            return true;
        default:
            return false;
        }

    case SubjectKind::CurrentTarget:
        switch (predicate)
        {
        case PredicateKind::Any:
        case PredicateKind::HealthPctBelow:
        case PredicateKind::MagickaPctBelow:
        case PredicateKind::StaminaPctBelow:
        case PredicateKind::AttackingPlayer:
        case PredicateKind::TargetOfPlayer:
            return true;
        default:
            return false;
        }

    default:
        return false;
    }
}

} // namespace ft
