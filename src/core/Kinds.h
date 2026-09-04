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
    Diseased,
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

} // namespace ft
