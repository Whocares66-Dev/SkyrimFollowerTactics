#pragma once
// Action dispatch: turn an ft::Decision into something that happens in Skyrim.

#include "core/Evaluator.h"
#include "game/Sensors.h"

namespace RE
{
class Actor;
}

namespace ft::game
{

// Why an action did not happen, for the log. The rule engine already explains
// why a rule did not *fire*; this explains why a rule that fired did not take
// effect, which is a different and much more suspicious failure.
enum class ActionResult : std::uint8_t
{
    Performed,
    NoSuchAction,   // not implemented in this phase
    MissingItem,    // the potion or spell vanished between snapshot and dispatch
    NoEquipManager, // the game singleton was unavailable
    Busy,           // the package pool was exhausted between evaluation and dispatch
    NoTarget        // a targeted spell with no enemy engaged
};

// Why the game refused a cast, in its own words. Kept separate from
// ActionResult because "she cannot afford it" and "she is mid-shout" are the
// same failure to us and completely different to a player.
[[nodiscard]] const char *CannotCastText(std::uint32_t reason) noexcept;

[[nodiscard]] const char *ToString(ActionResult r) noexcept;

// One action of a decision. A decision's steps are executed in order, each
// through here.
ActionResult Execute(const ft::Action &action, RE::Actor *actor, const PotionChoice &choice);

} // namespace ft::game
