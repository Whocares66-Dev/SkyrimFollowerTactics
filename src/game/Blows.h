#pragma once
// Bash and Power Bash: the block raised first, then the bash.
//
// WHY A SEQUENCE
// In the game's idle tree a bash is an attack made while blocking: bashStart
// is offered only to an actor who wants to block and is blocking, and the
// combat AI bashes the same way, its block raised by a CombatAnimation of the
// left attack action before its attack (dev/ATTACK.md "How the engine
// bashes"). The bash itself is the right attack action made from the block,
// which the tree resolves into bashStart, and which sets the bash attack
// state; the event sent straight to the graph was taken and set no such
// state (2026-09-15). The tree offers a power bash to the player alone, so a
// power bash is the same action from the block with bashPowerStart set as
// its event, as the combat AI's melee chooser makes one.
//
// So a request is steps: the hands free (the weapon drawn, no swing in
// progress), the block raised unless it is up already, the bash asked for
// once it is up, the bash watched until it ends, the block lowered if the
// request raised it. The follower's animation graph advances them: each
// event a step waits on -- their own swing ended, the block up and ready,
// the bash over -- queues a step at once; at the half-second turn the
// follower's AI would have the block down again between two steps. The
// turn is the backstop, for a deadline or an event that never came.

#include "core/Blows.h"
#include "core/Rule.h"

#include <cstdint>
#include <string_view>

namespace RE
{
class Actor;
class BGSAttackData;
} // namespace RE

namespace ft::game
{

enum class BlowRequest : std::uint8_t
{
    Started,        // the first step has run; rule.resolved says what came of it
    AlreadyInFlight // this follower has a blow in flight
};

// Start a bash, or a power bash, at the target, for the rule named, which
// rule.resolved names again when it is over. The caller has judged that
// what is in the hands bashes. Game thread.
[[nodiscard]] BlowRequest RequestBash(RE::Actor *actor, std::uint32_t targetId, bool power, int ruleIndex,
                                      std::string_view ruleName);

// Start a follower's power attack at the target, as their combat AI makes
// one: the right attack action carrying `event`, the attack their hands
// make (core/Blows.h, PowerAttackEvent), taken once their own swing is over
// and the target is inside the attack's strike angle, and followed by
// their graph's events to its hit and its end (core/Strike.h). Game
// thread.
[[nodiscard]] BlowRequest RequestStrike(RE::Actor *actor, std::uint32_t targetId, const char *event, int ruleIndex,
                                        std::string_view ruleName);

// Is this follower in the middle of a blow? Their rules wait meanwhile: a
// pin or a potion mid-blow would cut it off. Game thread.
[[nodiscard]] bool IsMidBlow(const RE::Actor *actor);

// Advance every request by a step where it can go on. Game thread.
void TickBlows(double now);

// End every request now, lowering a block one raised. For the save message.
void EndAllBlows(const char *why);

// Forget every request. For a game load: the handles mean nothing now.
void ResetBlows();

// The attack data an event names for this actor: their own record's, else
// their race's, as the engine finds it. Null where neither has it.
[[nodiscard]] const RE::BGSAttackData *AttackDataFor(RE::Actor *actor, const char *event);

// How far a swing has to reach to strike `to`, as the engine's melee test
// measures it: centre to centre, less both bodies (dev/ACTIONS.md 6).
[[nodiscard]] float ReachDistance(const RE::Actor *from, const RE::Actor *to);

// A blow with what the actor holds: the animation event that starts it,
// the stamina it costs, and how far it reaches, held against an enemy's
// ReachDistance. No event where the hands hold nothing for it. The race record's attack data carries the events and
// their multipliers (dev/ACTIONS.md 6).
struct BlowPlan
{
    const char *event{nullptr};
    float stamina{0.0f};
    float reach{0.0f};
    // Whether the follower has the perk the Settings page asks for this
    // blow, where it asks one (game/Settings.h). True when it asks none.
    bool perk{true};
    // A power attack's hands, which pick the player's attack action.
    ft::Swing swing{ft::Swing::None};
    [[nodiscard]] bool Possible() const noexcept
    {
        return event != nullptr;
    }
};
// A power attack, chosen by the hands, the right asked first: the right
// hand's blade or two-hander (attackPowerStartInPlace), both at once
// (...DualWield), else the left's blade (...LeftHand), else the fists (the
// right hand's event). None for a bow, a staff, a spell or a shield alone.
[[nodiscard]] BlowPlan PlanPowerAttack(RE::Actor *actor);
// A bash (bashStart) or a power bash (bashPowerStart), with what blocks: a
// shield or a torch in the left hand, or the right hand's weapon with the
// left hand empty.
[[nodiscard]] BlowPlan PlanBash(RE::Actor *actor, bool power);
// The blow a kind of action strikes; an empty plan for any other kind.
[[nodiscard]] BlowPlan PlanBlow(RE::Actor *actor, ft::ActionKind kind);

// Does the actor meet what the Settings page asks before a power bash: the
// Block tree's Power Bash perk, where it asks for one. True when it asks
// none, whatever is in the hands -- what they hold is a separate question,
// and PlanBash asks it.
[[nodiscard]] bool PowerBashPerkMet(RE::Actor *actor);

} // namespace ft::game
