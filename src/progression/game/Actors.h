#pragma once
// What Progression reads of an actor, and what it writes: the assigned
// points into the permanent modifiers (dev/PROGRESSION.md, "Engine
// approach"). Perks and spells are the views'; the base record is only read.
// Game thread only.

#include "progression/core/Companion.h"
#include "progression/core/Ids.h"
#include "progression/core/Skills.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace fp::game
{

// A person the player has recruited and not dismissed, alive, not the
// player. The teammate flag is what every follower framework sets (Follower
// Tactics' Tactics.cpp has the history); the engine's dismissed faction has
// the last word, since a framework may leave the flag set.
[[nodiscard]] bool IsFollower(RE::Actor *actor);
// Told to wait: a follower, but not on the journey right now.
[[nodiscard]] bool IsWaiting(RE::Actor *actor);
// One reference to one base: the only kind whose base perk list is theirs
// alone (DESIGN.md P0, "NPC base mutation affects other references").
[[nodiscard]] bool IsUniqueNpc(RE::Actor *actor);
[[nodiscard]] bool IsPerson(RE::Actor *actor);

// Every loaded follower near the player: the high process list.
[[nodiscard]] std::vector<RE::Actor *> LoadedFollowers();

// Skills as the engine has them without any modifier: what vanilla
// levelling gives, and what our training is added to.
[[nodiscard]] PerSkill<int> BaseSkills(RE::Actor *actor);
// Health, magicka and stamina: base value, no modifiers.
[[nodiscard]] PerAttribute<int> BaseAttributes(RE::Actor *actor);
// The full pool: base, permanent and temporary modifiers.
[[nodiscard]] int MaxMagicka(RE::Actor *actor);

// Writes `delta` into the actor's permanent modifiers: the console's modav.
void ApplyPoints(RE::Actor *actor, const Delta &delta);

// The perks on the actor's base record, as keys. Never written: what a
// companion holds beyond it is progression/game/PerkView.h's.
[[nodiscard]] std::unordered_set<FormKey, FormKeyHash> BasePerks(RE::Actor *actor);

// Takes off the actor the abilities `perk` gives (Magic Resistance,
// Recovery), unless their own record gives the same spell. What the
// engine's rank change does when a perk goes; done directly when releasing
// a companion, in case the save kept one (dev/ENGINE_PERKS.md).
void DropPerkAbilities(RE::Actor *actor, RE::BGSPerk *perk);

// The calendar's time now.

// Distance in game units; a very large number when either is missing.
[[nodiscard]] float DistanceToPlayer(RE::Actor *actor);

} // namespace fp::game
