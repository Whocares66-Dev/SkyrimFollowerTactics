#pragma once

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

// The power attack the hands allow, if any: the right hand's blade or
// two-hander, the left's alone, both at once, or the fists. A bow, a
// staff or a spell in the hand that would swing allows none.
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

} // namespace ft
