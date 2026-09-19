#pragma once

#include "Loadout.h"

#include <cstdint>

// What the hands can strike with: the rules of a swing and a bash, from
// what each hand holds. The game side describes the hands and maps the
// answer to the race's attack events; the rules live here so they can be
// tested, since they are vanilla's and not the engine's to answer.
namespace ft
{

// What one hand holds. A two-hander, a bow or a crossbow is the right
// hand's, with the left described as empty: the engine reports it from
// both hands, and the game side folds that before asking.
enum class Held : std::uint8_t
{
    Nothing,
    OneHander, // a sword, dagger, axe or mace
    TwoHander, // a greatsword, battleaxe or warhammer
    Bow,       // a bow or a crossbow
    Staff,
    Shield,
    Torch,
    Spell,
};

struct Hands
{
    Held right{Held::Nothing};
    Held left{Held::Nothing};
};

// The power attack the hands allow, if any, the right hand asked first and
// then the left: the right hand's blade or two-hander (both at once when the
// left holds a blade too), else the left's blade whatever the right holds,
// else the fists when both hands are empty. A bow, a staff, a spell or a
// shield swings nothing.
enum class Swing : std::uint8_t
{
    None,
    Right,
    Left,
    Both,
    Fists,
};
[[nodiscard]] Swing SwingWith(Hands hands) noexcept;

// Whether the hands can bash, which is whether they can block, by vanilla's
// rule: a shield or a torch in the left hand, else any weapon in the right
// hand with the left hand empty. A weapon alone in the left hand cannot
// block, nor can two hands each holding something, nor the fists, nor a
// spell hand.
[[nodiscard]] bool BashesWith(Hands hands) noexcept;

// The race's attack event for the swing: dual, in place, in place with
// the left hand; the fists swing in place. Null for no swing.
[[nodiscard]] const char *PowerAttackEvent(Swing swing) noexcept;
// bashStart, or bashPowerStart.
[[nodiscard]] const char *BashEvent(bool power) noexcept;

// The cost as the engine's own routine prices a power attack (26429 on
// 1.6.1170, dev/ACTIONS.md 6), before the perk entry point and the
// attack's own multiplier: the RIGHT hand's weapon's weight, 1 with none
// there, times fStaminaAttackWeaponMult, plus fStaminaAttackWeaponBase,
// times fPowerAttackStaminaPenalty -- 1, 20 and 2 in vanilla.
[[nodiscard]] float PowerAttackStamina(float rightWeight, float weaponMult, float weaponBase, float penalty) noexcept;

// Which hand a poison goes on: the right hand's weapon if it takes one
// and is clean, else the left's; None when neither. A staff takes none.
[[nodiscard]] Hand HandToPoison(bool rightTakes, bool rightPoisoned, bool leftTakes, bool leftPoisoned) noexcept;

// A weapon's charge after a soul gem: the soul's value, never negative,
// added to what is left and capped at the full charge.
[[nodiscard]] float ChargeAfterRecharge(float charge, float maxCharge, float soul) noexcept;

} // namespace ft
