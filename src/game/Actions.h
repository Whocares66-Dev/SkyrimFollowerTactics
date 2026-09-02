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
    NoSuchAction,  // not implemented in this phase
    MissingItem,   // the potion vanished between snapshot and dispatch
    NoEquipManager // the game singleton was unavailable
};

[[nodiscard]] const char *ToString(ActionResult r) noexcept;

ActionResult Execute(const ft::Decision &decision, RE::Actor *actor, const PotionChoice &choice);

} // namespace ft::game
