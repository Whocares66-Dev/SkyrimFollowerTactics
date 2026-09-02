#pragma once
// A rule is one row of the tactics grid: IF <condition> THEN <action> ON <target>.
// Deliberately flat and POD-ish so it round-trips to JSON without ceremony.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ft {

enum class ConditionKind : std::uint8_t {
    Always,
    SelfHealthPctBelow,
    SelfMagickaPctBelow,
    SelfStaminaPctBelow,
    SelfInBleedout,
    InCombat,
    PlayerHealthPctBelow,
    AllyHealthPctBelow,
    EnemyCountAtLeast,
    EnemyWithinDistance,
    TargetHealthPctBelow,

    COUNT
};

enum class TargetKind : std::uint8_t {
    Self,
    Player,
    CurrentTarget,
    NearestEnemy,
    LowestHealthEnemy,
    LowestHealthAlly,

    COUNT
};

enum class ActionKind : std::uint8_t {
    None,
    DrinkHealthPotion,
    DrinkMagickaPotion,
    DrinkStaminaPotion,
    SetCombatStyle,   // actionArg = style index into the ESP's CSTY palette
    SetAggression,    // actionArg = 0..3
    StopCombat,
    Flee,
    HoldPosition,

    COUNT
};

struct Rule {
    bool          enabled{true};
    ConditionKind condition{ConditionKind::Always};
    float         conditionArg{0.0f};
    TargetKind    target{TargetKind::Self};
    ActionKind    action{ActionKind::None};
    float         actionArg{0.0f};
    double        cooldown{0.0};  // seconds; 0 = only the global cooldown applies
    std::string   label;          // free text, shown in the UI, ignored by the engine
};

struct RuleSet {
    int               schemaVersion{1};
    std::string       name{"unnamed"};
    std::vector<Rule> rules;
};

// Which actions the current runtime can actually perform. src/game/ fills this
// in at startup. The UI greys out unsupported actions rather than letting
// someone author a rule that silently never fires -- see docs/PLAN.md 3.5.
struct Capabilities {
    std::array<bool, static_cast<std::size_t>(ActionKind::COUNT)> supported{};

    [[nodiscard]] bool Supports(ActionKind a) const noexcept {
        return supported[static_cast<std::size_t>(a)];
    }

    static Capabilities All() noexcept {
        Capabilities c;
        c.supported.fill(true);
        return c;
    }
};

}  // namespace ft
