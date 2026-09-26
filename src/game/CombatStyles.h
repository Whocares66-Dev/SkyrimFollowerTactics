#pragma once
// A follower's combat style as the player tunes it on the Combat Style tab
// (dev/COMBAT_AI.md "Combat styles"): a plus or minus on each number of
// whatever style their record has (core/CombatStyle.h).
//
// The combat AI reads a style's numbers straight from the object, at every
// place it uses one -- no function to hook, as Progression hooks the
// engine's reads of a value -- and a style is shared by every actor of a
// kind, so the follower is given a style object of their own: the record's
// bytes copied into memory of ours, the sums written in. It is no form: not
// in the form map, and it carries the record's own FormID, so anything the
// engine writes of it names the record's style. Their base record and their
// fight point at it while they are tuned and the setting is on; nothing of
// it is in the save, so the tuning lives in their co-save record and is put
// back when the tick first sees them after a load.

#include "core/CombatStyle.h"
#include "core/Snapshot.h"

#include <cstdint>
#include <optional>

namespace RE
{
class Actor;
class TESCombatStyle;
} // namespace RE

namespace ft::game
{

// The Combat Style tab's switches' controls (SheetRow::control): past the
// fields', whose sliders are their indices.
[[nodiscard]] constexpr int SwitchControl(ft::StyleSwitch which)
{
    return static_cast<int>(ft::kStyleFields) + static_cast<int>(which);
}
[[nodiscard]] constexpr std::optional<ft::StyleSwitch> SwitchOfControl(int control)
{
    const int index = control - static_cast<int>(ft::kStyleFields);
    if (index < 0 || index >= static_cast<int>(ft::kStyleSwitches))
        return std::nullopt;
    return static_cast<ft::StyleSwitch>(index);
}

// The panel's changes, queued to the game thread: one field's plus or
// minus, a switch turned over, and everything back to the record's.
void RequestStyleDelta(ft::ActorId id, ft::StyleField field, float delta);
void RequestStyleSwitch(ft::ActorId id, ft::StyleSwitch which);
void RequestStyleReset(ft::ActorId id);

// What the save keeps. Game thread.
[[nodiscard]] ft::StyleAdjustments StyleAdjustmentsOf(ft::ActorId id);

// The saved tuning, put on the follower. Game thread.
void AdoptCombatStyle(RE::Actor *actor, const ft::StyleAdjustments &adjustments);

// Each tick, for each follower: their style as their tuning and the setting
// say, put back if something else put another on their record -- a
// script's SetCombatStyle, whose style is then the one tuned. Cheap when
// nothing is off. Game thread.
void KeepCombatStyle(RE::Actor *actor);

// The setting turned on or off: every tuned follower's style put on or
// taken off. Queued to the game thread.
void RequestCombatStylesSynced();

// The tab's figures: each field on the record's style and its plus or
// minus. None while the setting is off. Game thread.
[[nodiscard]] std::optional<ft::StyleTuning> StyleTuningOf(RE::Actor *actor);

// Whether the style is a follower's copy of ours.
[[nodiscard]] bool IsTunedCopy(const RE::TESCombatStyle *style);

// A field's number on a style, by the field.
[[nodiscard]] float StyleValue(const RE::TESCombatStyle &style, ft::StyleField field);

// Before a load and on a new game: every record back on the style it had,
// and every tuning forgotten. The copies stay, for the fights that may
// still point at them. Game thread.
void ForgetCombatStyles();

} // namespace ft::game
