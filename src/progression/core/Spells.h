#pragma once
// Teaching a spell from a tome (dev/PROGRESSION.md, "Spells"): whether this
// companion can learn it, and in words why not. The game side reads the
// tome's taught spell, the spell's school, level and cost, and whether the
// companion knows it already (progression/game/Tomes.cpp). No Skyrim.

#include "progression/core/Ids.h"
#include "progression/core/Skills.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fp
{

struct SpellFacts
{
    FormKey spell;
    std::string name;
    std::optional<Skill> school; // none: not a school spell (a power, an ability, a scripted oddity)
    int minimumSkill{0};         // the costliest effect's own minimum skill: 0, 25, 50, 75, 100
    int cost{0};                 // magicka, before the companion's perks
    bool ordinary{true};         // a castable spell, not an ability, power or disease
};

enum class TeachBlock : std::uint8_t
{
    None,
    NotTeachable, // not an ordinary school spell
    Known,        // innate, or taught before
    Skill,        // below the spell's level
    Magicka,      // could never cast it
};

struct TeachStatus
{
    TeachBlock block{TeachBlock::None};
    int need{0};
    int have{0};
};

// "Novice", "Apprentice", "Adept", "Expert", "Master".
[[nodiscard]] std::string_view LevelName(int minimumSkill) noexcept;

// `skills` is base plus training; `maxMagicka` the companion's full pool.
[[nodiscard]] TeachStatus CanTeach(const SpellFacts &spell, bool known, const PerSkill<int> &skills, int maxMagicka);

} // namespace fp
