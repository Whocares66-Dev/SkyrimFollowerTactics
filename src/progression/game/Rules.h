#pragma once
// The game's own levelling numbers, read when they are wanted and not kept:
// the settings the player's levelling reads, and a skill's usage values
// through the engine's own reader (progression/core/Levelling.h, dev/ENGINE_SKILLS.md).
// A mod that changes them for the player changes them here. Game thread.

#include "progression/core/Levelling.h"
#include "progression/core/Skills.h"

#include <optional>

namespace fp::game
{

[[nodiscard]] Rules ReadRules();
// A skill's record's usage values (27244); none when its record has none,
// and then, as for the player, a use gives nothing.
[[nodiscard]] std::optional<SkillUsage> ReadSkillUsage(Skill skill);

// A setting's value now; `fallback` when no plugin or build has it. Also
// from the hit hook, off the game thread: the collection is only read.
[[nodiscard]] float SettingFloat(const char *name, float fallback);

// The race's bonus to a skill, 0 when it has none; and its starting health,
// magicka and stamina.
[[nodiscard]] int RaceSkillBonus(RE::Actor *actor, Skill skill);
[[nodiscard]] PerAttribute<int> RaceStart(RE::Actor *actor);

} // namespace fp::game
