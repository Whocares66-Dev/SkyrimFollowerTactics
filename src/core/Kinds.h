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

// How well armoured an actor is, by the damage their armour turns away
// rather than the rating the game shows, so a bandit in fur and a chief in
// plate are measured on one scale. The lines are from docs/CONDITIONS.md 3:
// under a quarter is Low -- animals, mages, dragons, bandits in fur --
// past 55% is High -- ebony and better, the cap being 80%.
enum class ArmorBand : std::uint8_t
{
    Low,
    Medium,
    High,

    COUNT
};

[[nodiscard]] constexpr ArmorBand BandOf(float damageReduction) noexcept
{
    if (damageReduction < 0.25f)
        return ArmorBand::Low;
    if (damageReduction < 0.55f)
        return ArmorBand::Medium;
    return ArmorBand::High;
}

} // namespace ft
