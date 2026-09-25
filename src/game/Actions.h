#pragma once
// Action dispatch: turn an ft::Decision into something that happens in Skyrim.

#include "core/Evaluator.h"
#include "game/Sensors.h"

#include <string_view>

namespace RE
{
class Actor;
}

namespace ft::game
{

// Why an action did not happen, for the log. The rule engine already explains
// why a rule did not *fire*; this explains why a rule that fired did not take
// effect, which is a different and much more suspicious failure.
// What a result is evidence of, per action, since the levels differ: a
// drink or an equip (Consume, the equip actions) says Performed when the
// engine's equip call was made and took the item -- for a queued equip
// its effect shows on the actor's next update, and nothing here proves
// the potion was drunk; a poison or a charge says Performed when the
// extra data was written and the ability refreshed; a cast, a scroll, a
// shout, a power, a power attack or a bash says Requested, and its
// outcome follows as rule.resolved when the lease or the run is over,
// which is when its cooldown starts (game/Tactics.cpp, inFlight).
enum class ActionResult : std::uint8_t
{
    Performed,
    Requested,      // asked of the AI -- a cast, a scroll, a shout, a power, a blow -- whose outcome follows
    NoSuchAction,   // no route for it here (core/Routes.h), or no records for a follower's cast
    MissingItem,    // the potion or spell vanished between snapshot and dispatch
    NoEquipManager, // the game singleton was unavailable
    Busy,           // a cast or a blow of theirs already in flight at dispatch
    NoTarget        // a targeted spell with no enemy engaged
};

// Why the game refused a cast, in its own words. Kept separate from
// ActionResult because "they cannot afford it" and "they are mid-shout" are the
// same failure to us and completely different to a player.
[[nodiscard]] const char *CannotCastText(std::uint32_t reason) noexcept;

[[nodiscard]] const char *ToString(ActionResult r) noexcept;

// One action of a decision. A decision's steps are executed in order, each
// through here. `target` is the step's: whom the rule aimed the action at.
// The action's form is the thing to use: a policy's has been resolved to
// the bottle it chose by the evaluator (ChosenForm), so a Strongest and a
// named potion arrive the same way. `ruleIndex` and `ruleName` are the rule's,
// for a request -- a cast, a power attack, a bash -- to name when it is over
// (rule.resolved).
ActionResult Execute(const ft::Action &action, ft::ActorId target, RE::Actor *actor, int ruleIndex,
                     std::string_view ruleName);

// The panel's Charge button: one copy of an enchanted weapon -- `row`, an
// Inventory row's list, or null for the plain stack -- charged with the
// weakest filled gem carried, on the game thread.
void RequestCharge(ft::ActorId id, std::uint32_t form, const void *row);

} // namespace ft::game
