#pragma once
// The kinds a condition can name beside its number: a status an actor is
// in, a kind of damage. Pure, like everything in core; the game side reads
// the actor and sets them, the evaluator only compares them.

#include <cstdint>

namespace ft
{

// What an actor is in the middle of, read off the actor each tick. One bit
// each in ActorTraits::status. docs/CONDITIONS.md 2 says what each reads.
enum class StatusKind : std::uint8_t
{
    Poisoned,
    Burning,
    Frostbitten,
    Shocked,
    Paralysed,
    Staggered,
    Fleeing,
    BleedingOut,
    Invisible,
    Ethereal,
    Blocking,
    Casting,
    Sneaking,

    COUNT
};

[[nodiscard]] constexpr std::uint32_t Bit(StatusKind kind) noexcept
{
    return 1u << static_cast<std::uint32_t>(kind);
}

// A kind of damage: what a resistance is against, what an attack was made
// with. How it arrived first -- a blow, an arrow or bolt, a spell of any
// kind -- then what a spell was. A Fire hit is Magic as well; an arrow is
// Ranged and not Melee. Nothing resists a blow or an arrow but armour,
// which is its own condition. Any is for Attacked by alone: hit with
// anything at all. Disease is not here: nothing attacks with it, and only
// the player has a resistance worth asking about. The order is the menu's.
enum class DamageKind : std::uint8_t
{
    Melee,
    Ranged,
    Magic,
    Fire,
    Frost,
    Shock,
    Poison,
    Any,

    COUNT
};

[[nodiscard]] constexpr std::uint8_t Bit(DamageKind kind) noexcept
{
    return static_cast<std::uint8_t>(1u << static_cast<unsigned>(kind));
}

} // namespace ft
