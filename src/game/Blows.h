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
// request raised it. The fast tick advances them (game/Tactics.cpp), every 50 ms
// while any is in flight: at the half-second turn the follower's AI would
// have the block down again between two steps.

#include <cstdint>
#include <string_view>

namespace RE
{
class Actor;
} // namespace RE

namespace ft::game
{

enum class BashRequest : std::uint8_t
{
    Started,       // the first step has run; rule.resolved says what came of it
    AlreadyBashing // this follower has one in flight
};

// Start a bash, or a power bash, at the target, for the rule named, which
// rule.resolved names again when it is over. The caller has judged that
// what is in the hands bashes. Game thread.
[[nodiscard]] BashRequest RequestBash(RE::Actor *actor, std::uint32_t targetId, bool power, int ruleIndex,
                                      std::string_view ruleName);

// Is this follower in the middle of one? Their rules wait meanwhile: a pin
// or a potion mid-bash would cut it off. Game thread.
[[nodiscard]] bool IsMidBash(const RE::Actor *actor);

// Is any in flight? Any thread: the pacing thread asks it to decide whether
// the fast tick is wanted.
[[nodiscard]] bool AnyBashInFlight() noexcept;

// Advance every request by a step where it can go on. Game thread.
void TickBashes(double now);

// End every request now, lowering a block one raised. For the save message.
void EndAllBashes(const char *why);

// Forget every request. For a game load: the handles mean nothing now.
void ResetBashes();

} // namespace ft::game
