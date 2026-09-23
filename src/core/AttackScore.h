#pragma once
// What an attack spell is worth per second of a follower's time, counting
// magicka as time. The engine scores a spell by what one cast does
// (magnitude x duration, dev/COMBAT_AI.md 2), and nothing of what it costs or
// how long it takes, so the biggest, dearest spell wins every time. A caster
// spends two things, time and magicka, and which is short changes as the
// fight goes on:
//
//   score = damage per cycle / (seconds per cycle + price x magicka per cycle)
//
// `price` is what a point of magicka is worth in seconds: nothing with the
// pool full, where the spell that does most per second wins, rising as it
// drains towards the time a point takes to come back, where the spell that
// does most per point does. Squared, so the pool's first half costs little
// and its last quarter a lot. A weapon's engine score is already per
// second and costs no magicka, so both are in one unit, and the combat
// style's multipliers stay on both as the preference they are.
//
// The cycle, for a spell released once: its charge time and the AI's hold
// before release (45354, dev/COMBAT_AI.md 4) -- the floor that keeps a spell
// with no charge time from dividing by nothing. The release animation is
// not in any record and is left out. For a stream: the engine's own scoring
// duration for one (fCombatMagicConcentrationScoreDuration), and the cost
// per second over it. A scroll or a staff costs no magicka.
//
// The game side reads the numbers (game/AiScore.cpp); what is made of them
// is here, where it is tested. No Skyrim.

namespace ft
{

struct MagickaPool
{
    float current{0.0f};
    float max{0.0f};
    // Points back per second, in a fight.
    float regenPerSecond{0.0f};
};

// A point is never priced above this many seconds: a pool that does not
// come back at all would otherwise make any cost infinite, and the spell
// that costs nothing -- a scroll, a staff -- the only choice.
inline constexpr double kMaxSecondsPerPoint = 5.0;

// Seconds a point of magicka is worth now: 0 with the pool full, up to one
// point's regeneration time with it empty, squared in between.
[[nodiscard]] double MagickaPrice(const MagickaPool &pool) noexcept;

// The AI's hold before releasing an attack spell: 0.5 s to 1.5 s by the
// combat style's offensive multiplier, never under 0.1 (45354).
[[nodiscard]] double HoldSeconds(double offensiveMult) noexcept;

struct SpellCycle
{
    double seconds{0.0};
    double magicka{0.0};
};

// A released spell: charge and hold, and its cost. A stream: the scoring
// duration, and its cost per second over it.
[[nodiscard]] SpellCycle ReleasedCycle(double chargeSeconds, double holdSeconds, double cost) noexcept;
[[nodiscard]] SpellCycle StreamCycle(double scoringSeconds, double costPerSecond) noexcept;

// A scroll is a backup for when magicka runs low: spent, not paid for, so
// no price holds it back, and it would otherwise be used first. Its score
// is taken at this share: none with the pool full, all of it with the pool
// empty, squared between as the price is.
[[nodiscard]] double ScrollReserve(const MagickaPool &pool) noexcept;

// What a cycle's damage is worth per second, magicka counted as time.
[[nodiscard]] double PerSecond(double damagePerCycle, const SpellCycle &cycle, double price) noexcept;

} // namespace ft
