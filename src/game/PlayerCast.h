#pragma once
// A cast on the player's own body, and a power or shout from their voice.
//
// WHY NOT THE FOLLOWER'S ROUTE
// A follower casts because a UseMagic package at the front of their stack
// gives their AI a reason to (game/Packages.h). The player runs no packages
// while the player is in control, and giving the AI the controls would take
// the character away for the length of every cast. What the player's own
// cast goes through is the input handler above the caster, and that is the
// entry used here: a ButtonEvent for the hand's attack control, or the shout
// control, handed straight to the handler as the player's own press is.
// Everything after the press is the engine's -- the animation, the charge,
// the magicka, the experience, the perks, the sound, the cast event other
// mods listen for. dev/PLAYER.md "The handler, read from the executable"
// is what the handler does with it, read from 1.6.1170:
//
//   - a press while sheathed only draws, so the draw is its own step;
//   - a press with nothing in flight asks the hand's caster to charge;
//   - a release at MagicCaster::State::kReady fires; earlier, it cancels;
//   - for a concentration spell, letting go ends the stream.
//
// So a cast is steps, each a state read on the fast tick as a bash's are
// (game/Blows.h): the spell lent a hand -- what the hand held is remembered
// and put back after -- the hands drawn if they were sheathed, one press,
// the caster watched for Ready, one release, the hand's spell-fire event
// waited for, the hand given back and the hands sheathed again if this drew
// them. A step that does not come within its window abandons the cast and
// gives the hand back. The voice is the same shape with the shout control:
// the power or shout selected, pressed, released, the voice's fire event
// waited for, the previous selection put back.
//
// A dual cast is the spell in each hand and the two controls pressed on one
// frame: the handler pairs the second press with the first into a dual
// press, and its release fires from the left hand's caster. The same
// pairing holds a SINGLE press back whenever a spell is in each hand, until
// a hold outlasts its window, so a single cast sends holds until the caster
// has begun.
//
// A scroll is a spell record that is also an item: lent to the hand as the
// item, cast by the same press, spent by the engine.
//
// The player's own press of the same button lands in the same state machine
// and stands: their release fires or cancels ours.

#include "core/Rule.h"

#include <cstdint>
#include <string_view>

namespace RE
{
class Actor;
} // namespace RE

namespace ft::game
{

// What the player's tactics can do: every action but Attack, which points
// a combat AI the player does not run at a target the player aims at
// themself. The blows go by the engine's own attack actions, the equips are
// plain equips (game/Pins.h, WearNow), the casts and the voice are this
// file's. The menu leaves Attack out on the player's page, and the
// evaluator reports it from a hand-edited profile as unsupported.
[[nodiscard]] bool PlayerSupports(ft::ActionKind kind) noexcept;

// Why the player cannot be acted for right now, or null: in dialogue, the
// fighting controls disabled by a scene, in furniture, mounted, in a kill
// move, knocked down, swimming, in beast form, the 3D not loaded. Each a
// state read now, not a timer. The tick holds the player's evaluation while
// one applies, and says so once.
[[nodiscard]] const char *PlayerHeld(RE::Actor *player);

enum class PlayerCastRequest : std::uint8_t
{
    Started,        // the first step has run; rule.resolved says what came of it
    AlreadyCasting, // one of ours is in flight; one at a time
    SpellMissing    // the form is not a spell, power or shout
};

[[nodiscard]] const char *ToString(PlayerCastRequest r) noexcept;

// Cast a spell from a hand, or from both at once for a dual cast (the
// caller has judged that the player can). sustainSeconds is how long a
// concentration spell's stream is held; zero takes the default. Game thread.
[[nodiscard]] PlayerCastRequest RequestPlayerCast(RE::Actor *player, std::uint32_t spellFormID, float sustainSeconds,
                                                  bool dualCast, int ruleIndex, std::string_view ruleName);

// A power (a spell record of type Power or Lesser Power) or a shout (a
// TESShout), from the voice: one tap of the shout control, which is the
// first word of a shout. Game thread.
[[nodiscard]] PlayerCastRequest RequestPlayerVoice(RE::Actor *player, std::uint32_t formID, int ruleIndex,
                                                   std::string_view ruleName);

// Is one of ours in flight? The player's rules wait meanwhile. Game thread.
[[nodiscard]] bool IsPlayerMidCast();

// Any thread: the pacing thread asks it to decide whether the fast tick is
// wanted.
[[nodiscard]] bool AnyPlayerCastInFlight() noexcept;

// Advance the cast in flight by a step where it can go on. Game thread.
void TickPlayerCasts(double now);

// End the cast in flight now: a press not yet released is released, the
// hands are given back. For the save message.
void EndAllPlayerCasts(const char *why);

// Forget the cast in flight. For a game load: nothing of the old game is
// touched.
void ResetPlayerCasts();

} // namespace ft::game
