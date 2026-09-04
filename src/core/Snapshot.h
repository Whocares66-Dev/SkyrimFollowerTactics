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

#include "Kinds.h"
#include "Loadout.h"

#include <algorithm>
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

// What is true of an actor beyond the numbers, read off the actor each
// tick: the statuses it is in, as one bit each. The same for the follower,
// the player, an ally and an enemy, so a Status condition asks the same
// question of any of them.
struct ActorTraits
{
    std::uint32_t status{0};

    [[nodiscard]] constexpr bool Has(StatusKind kind) const noexcept
    {
        return (status & Bit(kind)) != 0;
    }

    constexpr void Set(StatusKind kind) noexcept
    {
        status |= Bit(kind);
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
    ActorTraits traits{};
};

struct AllyView
{
    ActorId id{0};
    Stat health{};
    float distance{0.0f};
    bool inBleedout{false};
    ActorTraits traits{};
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

    // Every drinkable potion she carries, by form, with its count -- what a
    // DrinkPotion rule (one named potion) checks against. Names are display
    // and live on the game side.
    struct Carried
    {
        std::uint32_t form{0};
        int count{0};
    };
    std::vector<Carried> carried;

    [[nodiscard]] int CountOf(std::uint32_t form) const
    {
        for (const auto &c : carried)
            if (c.form == form)
                return c.count;
        return 0;
    }
};

// Spells the follower knows, and the ones whose effects are running right now.
//
// Both are FormIDs and both are opaque to core -- it never resolves them, it
// only asks whether one is in a list. That keeps the "is this buff already up"
// question answerable without core knowing what a spell is.
//
// This is the general form of what PotionStock's three bools do for restores.
// A buff like Oakflesh runs for sixty seconds, far longer than any cooldown
// worth choosing, so spacing cannot solve re-casting and only the effect list
// can: ask whether it is still running.
struct SpellState
{
    std::vector<std::uint32_t> known;
    std::vector<std::uint32_t> active;
    std::vector<std::uint32_t> equipped;

    // What each known spell costs HER, in magicka, with her perks and skill
    // already applied. The game computes it; core only compares it against
    // the magicka she has, so a cast rule she cannot afford is reported as
    // such instead of firing a package the AI will decline.
    struct Cost
    {
        std::uint32_t form{0};
        float magicka{0.0f};
    };
    std::vector<Cost> costs;

    // Zero for a spell with no recorded cost, so a snapshot that does not
    // carry costs (a test, an older sensor) never blocks a cast.
    [[nodiscard]] float CostOf(std::uint32_t form) const
    {
        for (const auto &c : costs)
            if (c.form == form)
                return c.magicka;
        return 0.0f;
    }

    [[nodiscard]] bool Knows(std::uint32_t form) const
    {
        return std::find(known.begin(), known.end(), form) != known.end();
    }

    [[nodiscard]] bool IsActive(std::uint32_t form) const
    {
        return std::find(active.begin(), active.end(), form) != active.end();
    }

    [[nodiscard]] bool IsEquipped(std::uint32_t form) const
    {
        return std::find(equipped.begin(), equipped.end(), form) != equipped.end();
    }
};

struct Snapshot
{
    ActorId self{0};
    double now{0.0}; // seconds, monotonic

    Stat health{};
    Stat magicka{};
    Stat stamina{};

    bool inCombat{false};
    // The edges: this is the first evaluation of a fight, or the one
    // farewell evaluation after it. On the farewell pass only CombatEnds
    // holds -- see PredicateKind.
    bool combatBegan{false};
    bool combatEnded{false};
    bool inBleedout{false};
    bool weaponDrawn{false};
    bool sneaking{false};
    ActorTraits traits{};

    Stat playerHealth{};
    float distanceToPlayer{0.0f};
    bool playerInCombat{false};
    ActorTraits playerTraits{};

    ActorId currentTarget{0};

    std::vector<EnemyView> enemies;
    std::vector<AllyView> allies;

    PotionStock potions;
    SpellState spells;

    // What she could hold or wear, as the pin book describes it -- weapons,
    // shields, torches, armour, ammunition, and the spells she knows -- and
    // what is pinned right now, by the panel or by a rule. The equip actions
    // read both: a thing not here cannot be pinned, and one already pinned
    // in the hands asked for is done, so the rule falls through.
    std::vector<Holdable> loadout;
    std::vector<Pin> pins;
};

} // namespace ft
