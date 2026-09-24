#pragma once
// The follower's side of the combat AI's choice of weapon and spell
// (dev/COMBAT_AI.md "What we change"). The engine rescores every entry of a
// fighter's combat inventory once a second and takes the best of each
// category; the score hook in Pins.cpp answers for each entry, and for a
// follower -- a player teammate, never the player, never anyone else --
// its answer is this, before the pins and bans have their say:
//
//   their spells stand down while a rule of theirs waits on the cast in
//   their hands, or holds a cast record, so the AI finishes what it is
//   casting, starts nothing else, and the rule gets its turn
//
//   an attack weapon's score is scaled by what their perks and damage
//   effects make of it against the enemy they fight, which the engine's
//   own damage figure leaves out
//
//   an attack spell's the same -- a scroll's, a staff's enchantment's --
//   through the magnitude perks, and zero
//   against an enemy whom every hostile effect's conditions spare (a
//   paralysis on an automaton): the engine counts resistances, not these
//
//   an attack spell's score is varied, and held, and lowered for a while
//   after it is cast (core/Variety.h)
//
// Everything else -- their heals, wards, buffs, and every entry of every
// actor who is not a follower -- is the engine's own answer, as corrected
// in src/fix/. The stand-down is always on; the rest is the Settings
// page's Varied AI choices.

#include <cstdint>
#include <vector>

namespace RE
{
class Actor;
class CombatController;
class CombatInventoryItem;
} // namespace RE

namespace ft::game
{

// The score to answer for one entry of a follower's combat inventory, from
// the engine's. Any thread: the AI scores on its own.
[[nodiscard]] float FollowerScore(RE::CombatInventoryItem *entry, RE::CombatController *controller, RE::Actor *actor,
                                  float engine);

// The answer given for an entry after the pins and bans have had their say:
// what the cast line lists, so a banned spell reads 0 there. Debug only.
void NoteAnswer(RE::CombatInventoryItem *entry, RE::CombatController *controller, RE::Actor *actor, float answer);

// The tick's word, once per tick, on which followers have a rule waiting
// on their own cast (core's WaitsOnOwnCast): the set replaces the last.
void SetWaitingOnOwnCast(std::vector<std::uint32_t> followers);

// Whether a follower's self-targeting damage spell may be equipped where the
// engine's equip check refused it. The engine files a spell under a caster
// by its best-matching effect, and a spell cast on oneself whose harm has
// no entry is filed under whatever else it has -- Cold Fire Storm under the
// script caster -- whose check was made for another kind of spell. With
// "Use self-targeting damage spells" on: an attack spell cast on oneself
// with damage rings, last scored above 0 (an enemy in reach), affordable,
// the caster not fleeing. Any thread.
[[nodiscard]] bool SelfDamageMayEquip(RE::CombatInventoryItem *entry, RE::CombatController *controller,
                                      RE::Actor *actor);

// Once, at data load: hear every spell cast, for the penalty and the hold.
void WatchCasts();

// A load or a new game: every follower's draws, casts and waits forgotten.
void ResetAiScores();

} // namespace ft::game
