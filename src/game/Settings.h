#pragma once
// What the Settings page requires of a follower, beside the tactics switch.
//
// Each of the three gates a check that is otherwise the game's own rule: the
// combat style's dual wield flag, the school's Dual Casting perk, the Block
// tree's Power Bash perk. An NPC needs none of them in vanilla -- the idle
// tree asks the Power Bash perk of the player alone, and the combat style is
// a hint to the AI, not a law -- so each is the player's choice, and what
// they choose is saved with the game (game/Profiles.h, dev/PROFILES.md).
//
// And one that is no gate: varied AI choices, which has the AI's
// score for a follower's weapons and attack spells answered by ours
// (game/AiScore.h).
//
// Read by the tick building a snapshot, by the panel drawing a menu and by
// the AI's score; written by the panel and by a load. Atomics, so a read
// costs nothing.

#include "core/Profile.h"

namespace ft::game
{

[[nodiscard]] ft::Settings CurrentSettings();

// Logs what changed, once per change. Game thread, or a load's callback.
void SetSettings(const ft::Settings &settings);

} // namespace ft::game
