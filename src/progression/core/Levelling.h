#pragma once
// The player's levelling rules, for a companion (dev/PROGRESSION.md,
// "Learning by doing"; dev/ENGINE_SKILLS.md). A use of a skill gives skill
// XP; enough skill XP raises the skill a level; each skill-up gives
// character XP; enough character XP is a level.
//
// The numbers are the game's own, read on the game side (progression/game/Rules.cpp)
// each time they are wanted: the level-up settings, and a skill's usage
// values from its record. So a mod that changes them for the player changes
// them for companions. The formulas are the engine's, copied: its functions for
// them read the player's own skills and level instead of taking them. No
// Skyrim.

#include "progression/core/Skills.h"

namespace fp
{

// A skill's usage values (its record's AVSK), as the engine's 27244 reads
// them for the player.
struct SkillUsage
{
    double useMult{1.0};
    double useOffset{0.0};
    double improveMult{1.0};
    double improveOffset{0.0};
};

// The level-up settings.
struct Rules
{
    double skillUseCurve{1.95}; // fSkillUseCurve (Skyrim.esm's; the executable's default is 1.25)
    double xpPerSkillRank{1.0}; // fXPPerSkillRank
    double levelUpBase{75.0};   // fXPLevelUpBase
    double levelUpMult{25.0};   // fXPLevelUpMult
    int attributePerLevel{10};  // iAVDhmsLevelUp
    int skillStart{15};         // iAVDSkillStart
    int skillCap{100};          // a constant in the engine's code
    int levelsAbovePlayer{5};   // ours: how far past the player learning can take them
};

// Skill XP from one use worth `points`: points x use multiplier + use
// offset (41561).
[[nodiscard]] double SkillXp(const SkillUsage &u, double points) noexcept;

// Skill XP from `level` to level + 1: improve multiplier x level ^
// fSkillUseCurve + improve offset (41561, 41573); 0 at the cap.
[[nodiscard]] double SkillThreshold(const Rules &r, const SkillUsage &u, int level) noexcept;

// Character XP a skill-up to `level` gives: level x fXPPerSkillRank (41561).
// Also what a level is worth when it is taken back or bought with the
// reassigning pool.
[[nodiscard]] double XpForSkillLevel(const Rules &r, int level) noexcept;

// Character XP from `level` to level + 1: fXPLevelUpBase + fXPLevelUpMult x
// level (41560).
[[nodiscard]] double LevelThreshold(const Rules &r, int level) noexcept;

// Character XP from level 1 to `level`.
[[nodiscard]] double XpToReach(const Rules &r, int level) noexcept;

// The level a character XP total reaches, from level 1.
[[nodiscard]] int LevelFor(const Rules &r, double xp) noexcept;

} // namespace fp
