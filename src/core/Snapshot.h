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

namespace ft {

using ActorId = std::uint32_t;  // FormID, resolved via ResolveFormID on load

struct Stat {
    float current{0.0f};
    float max{0.0f};

    [[nodiscard]] constexpr float Pct() const noexcept {
        return max > 0.0f ? current / max : 0.0f;
    }
};

struct EnemyView {
    ActorId id{0};
    Stat    health{};
    float   distance{0.0f};
    bool    isCasting{false};
    bool    isAttackingPlayer{false};
    bool    hasLineOfSight{false};
};

struct AllyView {
    ActorId id{0};
    Stat    health{};
    float   distance{0.0f};
    bool    inBleedout{false};
};

// Counts and best-available magnitude per potion kind. Populated by an
// inventory scan in src/game/, which is expensive -- see the sensor gating
// note in docs/PLAN.md section 3.3.
struct PotionStock {
    int   healthCount{0};
    float bestHealthMagnitude{0.0f};
    int   magickaCount{0};
    float bestMagickaMagnitude{0.0f};
    int   staminaCount{0};
    float bestStaminaMagnitude{0.0f};
};

struct Snapshot {
    ActorId self{0};
    double  now{0.0};  // seconds, monotonic

    Stat health{};
    Stat magicka{};
    Stat stamina{};

    bool inCombat{false};
    bool inBleedout{false};
    bool weaponDrawn{false};
    bool sneaking{false};

    Stat  playerHealth{};
    float distanceToPlayer{0.0f};
    bool  playerInCombat{false};

    ActorId currentTarget{0};

    std::vector<EnemyView> enemies;
    std::vector<AllyView>  allies;

    PotionStock potions;
};

}  // namespace ft
