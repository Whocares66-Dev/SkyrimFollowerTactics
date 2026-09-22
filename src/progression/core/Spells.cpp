#include "progression/core/Spells.h"

namespace fp
{

std::string_view LevelName(int minimumSkill) noexcept
{
    if (minimumSkill >= 100)
        return "Master";
    if (minimumSkill >= 75)
        return "Expert";
    if (minimumSkill >= 50)
        return "Adept";
    if (minimumSkill >= 25)
        return "Apprentice";
    return "Novice";
}

TeachStatus CanTeach(const SpellFacts &spell, bool known, const PerSkill<int> &skills, int maxMagicka)
{
    if (!spell.ordinary || !spell.school || !IsSchool(*spell.school))
        return {TeachBlock::NotTeachable, 0, 0};
    if (known)
        return {TeachBlock::Known, 0, 0};
    const int have = skills[Index(*spell.school)];
    if (have < spell.minimumSkill)
        return {TeachBlock::Skill, spell.minimumSkill, have};
    if (spell.cost > maxMagicka)
        return {TeachBlock::Magicka, spell.cost, maxMagicka};
    return {TeachBlock::None, 0, 0};
}

} // namespace fp
