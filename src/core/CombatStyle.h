#pragma once
// A follower's combat style as the player tunes it on the Combat Style tab:
// a plus or minus on each of the style's numbers, over whatever style their
// record has (dev/COMBAT_AI.md "Combat styles"). The game puts the sum on a
// copy of the style made in memory for them (game/CombatStyles.h); this is
// the arithmetic, and what the co-save keeps (core/Profile.h).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace ft
{

// The numbers a style has that the tab shows, in the tab's order. Not among
// them: group offensive, which the engine applies only to an actor
// fighting the player; avoid threat and special attack, the dodge and
// special attack chances, which the Creation Kit wiki calls unused; and the
// flight fields, a dragon's (dev/COMBAT_AI.md "What reads each field").
enum class StyleField : std::uint8_t
{
    Offensive,
    Defensive,
    MeleeScore,
    MagicScore,
    RangedScore,
    StaffScore,
    ShoutScore,
    UnarmedScore,
    AttackStaggered,
    PowerAttackStaggered,
    PowerAttackBlocking,
    Bash,
    BashRecoiled,
    BashAttacking,
    BashPowerAttacking,
    Circle,
    Fallback,
    FlankDistance,
    StalkTime,
    Strafe,
    COUNT
};

inline constexpr std::size_t kStyleFields = static_cast<std::size_t>(StyleField::COUNT);

// One number per field, by the field's index.
using StyleValues = std::array<float, kStyleFields>;

// The two scales the load order's styles keep to: the chances and movement
// multipliers run 0 to 1 and move by hundredths, the score and attack
// multipliers 0 to 10 and move by tenths.
[[nodiscard]] float StyleMax(StyleField field);
[[nodiscard]] float StyleStep(StyleField field);

// The flags the tab makes switches of: Allow Dual Wielding, and Flanking,
// which picks the close-range movement (flank distance and stalk time
// rather than circle and fallback).
enum class StyleSwitch : std::uint8_t
{
    DualWield,
    Flanking,
    COUNT
};

inline constexpr std::size_t kStyleSwitches = static_cast<std::size_t>(StyleSwitch::COUNT);

// The wire name (dev/PROFILES.md), and back; a switch's sits
// beside the fields'.
[[nodiscard]] std::string_view WireName(StyleField field);
[[nodiscard]] std::optional<StyleField> StyleFieldFromWireName(std::string_view name);
[[nodiscard]] std::string_view WireName(StyleSwitch which);
[[nodiscard]] std::optional<StyleSwitch> StyleSwitchFromWireName(std::string_view name);

// A plus or minus to the step, and the value it makes of `base`, held
// within the scale. A base already past the top of its scale -- a style
// made outside it -- keeps its own top.
[[nodiscard]] float SnapStyleDelta(StyleField field, float delta);
[[nodiscard]] float AdjustedStyleValue(StyleField field, float base, float delta);

// How far the plus or minus can go from `base`: down to 0, up to the top.
[[nodiscard]] std::pair<float, float> StyleDeltaRange(StyleField field, float base);

[[nodiscard]] bool AnyStyleDelta(const StyleValues &deltas);

// Everything the player changed of a follower's style: a plus or minus on
// each number, and their word on each switch, none for the record's.
struct StyleAdjustments
{
    StyleValues deltas{};
    std::array<std::optional<bool>, kStyleSwitches> switches{};

    bool operator==(const StyleAdjustments &) const = default;
};

[[nodiscard]] bool AnyStyleAdjustment(const StyleAdjustments &adjustments);

// A switch as it stands: the player's word, or the record's.
[[nodiscard]] bool SwitchOn(const StyleAdjustments &adjustments, StyleSwitch which, bool record);

// A click on a switch: the flag the other way, and no word at all where
// that is the record's, so switching it back is no change.
[[nodiscard]] std::optional<bool> ToggledSwitch(const StyleAdjustments &adjustments, StyleSwitch which, bool record);

// The tab's figures for one follower: each field and switch on their
// record's style, and what the player changed.
struct StyleTuning
{
    StyleValues base{};
    std::array<bool, kStyleSwitches> baseSwitches{};
    StyleAdjustments adjustments;
};

} // namespace ft
