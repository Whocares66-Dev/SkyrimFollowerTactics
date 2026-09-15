#pragma once
// Bash and Power Bash: the block raised first, then the bash.
//
// WHY A SEQUENCE
// In the game's idle tree a bash is an attack made while blocking: bashStart
// is offered only to an actor who wants to block and is blocking, and the
// combat AI bashes the same way, its block raised by a CombatAnimation of the
// left attack action before its attack (docs/ATTACK.md "How the engine
// bashes"). A bash event sent to a follower who is not blocking is one the
// tree would never choose. Power Bash goes the same way; the tree offers a
// power bash to the player alone, so it is sent to the graph as an event
// too, but from the block.
//
// So a request is steps: the hands free (the weapon drawn, no swing in
// progress), the block raised unless it is up already, the bash sent once it
// is up, the bash watched until it ends, the block lowered if the request
// raised it. The fast tick advances them (game/Tactics.cpp), every 50 ms
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

// Start a bash, or a power bash, for the rule named, which rule.resolved
// names again when it is over. The caller has judged that what is in the
// hands bashes. Game thread.
[[nodiscard]] BashRequest RequestBash(RE::Actor *actor, bool power, int ruleIndex, std::string_view ruleName);

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
