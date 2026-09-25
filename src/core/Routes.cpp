#include "core/Routes.h"

namespace ft
{

// Every kind is named, and none is left to a default: a kind added to the
// rule and not here falls to the end and has no route for anyone, which
// tests/test_routes.cpp catches.
Route RouteOf(ActionKind kind, Performer performer) noexcept
{
    const bool player = performer == Performer::Player;
    switch (kind)
    {
    case ActionKind::None:
    case ActionKind::COUNT:
        return Route::None;
    case ActionKind::DrinkStrongest:
    case ActionKind::DrinkWeakest:
    case ActionKind::DrinkAny:
    case ActionKind::DrinkPotion:
    case ActionKind::EatStrongestFood:
    case ActionKind::EatWeakestFood:
    case ActionKind::EatAnyFood:
    case ActionKind::EatFood:
    case ActionKind::EatStrongestIngredient:
    case ActionKind::EatWeakestIngredient:
    case ActionKind::EatIngredient:
        return Route::Consume;
    case ActionKind::ApplyStrongest:
    case ActionKind::ApplyWeakest:
    case ActionKind::ApplyAny:
    case ActionKind::ApplyPoison:
        return Route::ApplyPoison;
    case ActionKind::ChargeStrongestSoulGem:
    case ActionKind::ChargeWeakestSoulGem:
    case ActionKind::ChargeSoulGem:
        return Route::Charge;
    case ActionKind::CastSpell:
    case ActionKind::UseScroll:
        return player ? Route::CastPress : Route::CastRecord;
    case ActionKind::UsePower:
    case ActionKind::Shout:
        return player ? Route::VoicePress : Route::VoiceRecord;
    case ActionKind::EquipWeapon:
    case ActionKind::EquipArrows:
    case ActionKind::EquipStrongestArrows:
    case ActionKind::EquipWeakestArrows:
    case ActionKind::EquipSpell:
    case ActionKind::EquipArmor:
        // A pin is a leash on a combat AI the player does not run.
        return player ? Route::Wear : Route::Pin;
    case ActionKind::Attack:
        return player ? Route::None : Route::Target;
    case ActionKind::Bash:
    case ActionKind::PowerBash:
        return Route::Bash;
    case ActionKind::PowerAttack:
        return Route::Strike;
    }
    return Route::None;
}

} // namespace ft
