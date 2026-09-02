#pragma once
// Snapshot: everything the rule engine is allowed to know about the world.
//
// HARD RULE: no RE:: types below this line, ever. This header must compile
// with a plain C++23 compiler and no Skyrim, no SKSE, no CommonLibSSE.
// That is the only reason any of this is testable -- there is no headless
// test harness for Skyrim, so the logic has to not need the game.
//
// src/game/ builds one of these per follower per tick. Everything in
// src/core/ consumes it and nothing else.

#include <cstdint>
#include <vector>

namespace ft
{

using ActorId = std::uint32_t; // FormID, resolved via ResolveFormID on load

// The player is always 0x14. Named here so the evaluator does not carry a
// bare magic number, and so src/game/ and src/core/ agree on it.
inline constexpr ActorId kPlayerFormID = 0x14;

struct Stat
{
    float current{0.0f};
    float max{0.0f};

    [[nodiscard]] constexpr float Pct() const noexcept
    {
        return max > 0.0f ? current / max : 0.0f;
    }
};

struct EnemyView
{
    ActorId id{0};
    Stat health{};
    float distance{0.0f};
    bool isCasting{false};
    bool isAttackingPlayer{false};
    bool hasLineOfSight{false};
};

struct AllyView
{
    ActorId id{0};
    Stat health{};
    float distance{0.0f};
    bool inBleedout{false};
};

// Counts and best-available magnitude per potion kind. Populated by an
// inventory scan in src/game/, which is expensive -- see the sensor gating
// note in docs/PLAN.md section 3.3.
struct PotionStock
{
    int healthCount{0};
    float bestHealthMagnitude{0.0f};
    int magickaCount{0};
    float bestMagickaMagnitude{0.0f};
    int staminaCount{0};
    float bestStaminaMagnitude{0.0f};

    // True while a restore effect is still running on the follower.
    //
    // Vanilla alchemy Restore Health is INSTANT -- duration 0, nothing lingers --
    // so on an unmodded game these stay false and the settle time in
    // MinimumCooldown does the spacing. But potion overhauls commonly convert
    // restores to over-time effects (Potions Restore Over Time, Apothecary, and
    // others), and there a fixed settle is guesswork: the dose might run for ten
    // seconds. Asking the game whether the previous dose is still working is
    // exact, and it costs one walk of the active-effect list we already have.
    bool healthEffectActive{false};
    bool magickaEffectActive{false};
    bool staminaEffectActive{false};
};

struct Snapshot
{
    ActorId self{0};
    double now{0.0}; // seconds, monotonic

    Stat health{};
    Stat magicka{};
    Stat stamina{};

    bool inCombat{false};
    bool inBleedout{false};
    bool weaponDrawn{false};
    bool sneaking{false};

    Stat playerHealth{};
    float distanceToPlayer{0.0f};
    bool playerInCombat{false};

    ActorId currentTarget{0};

    std::vector<EnemyView> enemies;
    std::vector<AllyView> allies;

    PotionStock potions;
};

} // namespace ft
