#include "progression/core/Skills.h"

namespace fp
{
namespace
{

struct SkillText
{
    std::string_view name;
    std::string_view key;
};

constexpr std::array<SkillText, kSkillCount> kSkillText{{
    {"One-Handed", "OneHanded"},
    {"Two-Handed", "TwoHanded"},
    {"Archery", "Archery"},
    {"Block", "Block"},
    {"Smithing", "Smithing"},
    {"Heavy Armor", "HeavyArmor"},
    {"Light Armor", "LightArmor"},
    {"Pickpocket", "Pickpocket"},
    {"Lockpicking", "Lockpicking"},
    {"Sneak", "Sneak"},
    {"Alchemy", "Alchemy"},
    {"Speech", "Speech"},
    {"Alteration", "Alteration"},
    {"Conjuration", "Conjuration"},
    {"Destruction", "Destruction"},
    {"Illusion", "Illusion"},
    {"Restoration", "Restoration"},
    {"Enchanting", "Enchanting"},
}};

constexpr std::array<std::string_view, kAttributeCount> kAttributeName{"Health", "Magicka", "Stamina"};

} // namespace

const std::array<Skill, kSkillCount> &AllSkills() noexcept
{
    static constexpr std::array<Skill, kSkillCount> all{
        Skill::OneHanded,  Skill::TwoHanded,   Skill::Archery,    Skill::Block,       Skill::Smithing,
        Skill::HeavyArmor, Skill::LightArmor,  Skill::Pickpocket, Skill::Lockpicking, Skill::Sneak,
        Skill::Alchemy,    Skill::Speech,      Skill::Alteration, Skill::Conjuration, Skill::Destruction,
        Skill::Illusion,   Skill::Restoration, Skill::Enchanting};
    return all;
}

std::string_view Name(Skill s) noexcept
{
    return kSkillText[Index(s)].name;
}

std::string_view Name(Attribute a) noexcept
{
    return kAttributeName[Index(a)];
}

std::string_view Key(Skill s) noexcept
{
    return kSkillText[Index(s)].key;
}

std::optional<Skill> SkillFromKey(std::string_view key) noexcept
{
    for (std::size_t i = 0; i < kSkillCount; ++i)
        if (kSkillText[i].key == key)
            return static_cast<Skill>(i);
    return std::nullopt;
}

std::string_view Key(Attribute a) noexcept
{
    return kAttributeName[Index(a)];
}

std::optional<Attribute> AttributeFromKey(std::string_view key) noexcept
{
    for (std::size_t i = 0; i < kAttributeCount; ++i)
        if (kAttributeName[i] == key)
            return static_cast<Attribute>(i);
    return std::nullopt;
}

bool IsSchool(Skill s) noexcept
{
    switch (s)
    {
    case Skill::Alteration:
    case Skill::Conjuration:
    case Skill::Destruction:
    case Skill::Illusion:
    case Skill::Restoration:
        return true;
    default:
        return false;
    }
}

std::optional<Skill> SkillFromActorValue(int av) noexcept
{
    if (av < 6 || av >= 6 + static_cast<int>(kSkillCount))
        return std::nullopt;
    return static_cast<Skill>(av - 6);
}

} // namespace fp
