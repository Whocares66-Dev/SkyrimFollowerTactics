// How each action is performed, by the follower and by the player
// (core/Routes.h).

#include <catch2/catch_test_macros.hpp>

#include "core/Routes.h"

#include <cstddef>

using namespace ft;

namespace
{

Route Follower(ActionKind kind)
{
    return RouteOf(kind, Performer::Follower);
}

Route Player(ActionKind kind)
{
    return RouteOf(kind, Performer::Player);
}

} // namespace

TEST_CASE("every action a rule can hold has a route for a follower", "[routes]")
{
    // A kind added to the rule and forgotten here has none.
    for (std::size_t i = 1; i < static_cast<std::size_t>(ActionKind::COUNT); ++i)
        REQUIRE(Follower(static_cast<ActionKind>(i)) != Route::None);
    REQUIRE(Follower(ActionKind::None) == Route::None);
    REQUIRE(Player(ActionKind::None) == Route::None);
    REQUIRE(Follower(ActionKind::COUNT) == Route::None);
}

TEST_CASE("the player does all but Attack, and only their casts and equips go another way", "[routes]")
{
    for (std::size_t i = 1; i < static_cast<std::size_t>(ActionKind::COUNT); ++i)
    {
        const auto kind = static_cast<ActionKind>(i);
        switch (Follower(kind))
        {
        case Route::CastRecord:
            REQUIRE(Player(kind) == Route::CastPress);
            break;
        case Route::VoiceRecord:
            REQUIRE(Player(kind) == Route::VoicePress);
            break;
        case Route::Pin:
            REQUIRE(Player(kind) == Route::Wear);
            break;
        case Route::Target:
            REQUIRE(Player(kind) == Route::None);
            break;
        default:
            REQUIRE(Player(kind) == Follower(kind));
        }
    }
}

TEST_CASE("the kinds a policy resolves to one thing share their thing's route", "[routes]")
{
    for (const ActionKind kind :
         {ActionKind::DrinkStrongest, ActionKind::DrinkWeakest, ActionKind::DrinkAny, ActionKind::DrinkPotion,
          ActionKind::EatStrongestFood, ActionKind::EatWeakestFood, ActionKind::EatAnyFood, ActionKind::EatFood,
          ActionKind::EatStrongestIngredient, ActionKind::EatWeakestIngredient, ActionKind::EatIngredient})
        REQUIRE(Follower(kind) == Route::Consume);
    for (const ActionKind kind :
         {ActionKind::ApplyStrongest, ActionKind::ApplyWeakest, ActionKind::ApplyAny, ActionKind::ApplyPoison})
        REQUIRE(Follower(kind) == Route::ApplyPoison);
    for (const ActionKind kind :
         {ActionKind::ChargeStrongestSoulGem, ActionKind::ChargeWeakestSoulGem, ActionKind::ChargeSoulGem})
        REQUIRE(Follower(kind) == Route::Charge);
    for (const ActionKind kind : {ActionKind::EquipWeapon, ActionKind::EquipArrows, ActionKind::EquipStrongestArrows,
                                  ActionKind::EquipWeakestArrows, ActionKind::EquipSpell, ActionKind::EquipArmor})
        REQUIRE(Follower(kind) == Route::Pin);
    REQUIRE(Follower(ActionKind::CastSpell) == Route::CastRecord);
    REQUIRE(Follower(ActionKind::UseScroll) == Route::CastRecord);
    REQUIRE(Follower(ActionKind::UsePower) == Route::VoiceRecord);
    REQUIRE(Follower(ActionKind::Shout) == Route::VoiceRecord);
    REQUIRE(Follower(ActionKind::Attack) == Route::Target);
    REQUIRE(Follower(ActionKind::Bash) == Route::Bash);
    REQUIRE(Follower(ActionKind::PowerBash) == Route::Bash);
}

TEST_CASE("a power attack is one route for both: the action a follower's combat AI takes", "[routes]")
{
    REQUIRE(Follower(ActionKind::PowerAttack) == Route::Strike);
    REQUIRE(Player(ActionKind::PowerAttack) == Route::Strike);
}
