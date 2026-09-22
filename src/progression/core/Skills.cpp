#include "progression/core/Skills.h"

#include "core/I18n.h"

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
    {N_("One-Handed"), "OneHanded"},
    {N_("Two-Handed"), "TwoHanded"},
    {N_("Archery"), "Archery"},
    {N_("Block"), "Block"},
    {N_("Smithing"), "Smithing"},
    {N_("Heavy Armor"), "HeavyArmor"},
    {N_("Light Armor"), "LightArmor"},
    {N_("Pickpocket"), "Pickpocket"},
    {N_("Lockpicking"), "Lockpicking"},
    {N_("Sneak"), "Sneak"},
    {N_("Alchemy"), "Alchemy"},
    {N_("Speech"), "Speech"},
    {N_("Alteration"), "Alteration"},
    {N_("Conjuration"), "Conjuration"},
    {N_("Destruction"), "Destruction"},
    {N_("Illusion"), "Illusion"},
    {N_("Restoration"), "Restoration"},
    {N_("Enchanting"), "Enchanting"},
}};

constexpr std::array<std::string_view, kAttributeCount> kAttributeName{N_("Health"), N_("Magicka"),
                                                                         N_("Stamina")};

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

std::optional<Skill> SkillFromActorValue(int av) noexcept
{
    if (av < 6 || av >= 6 + static_cast<int>(kSkillCount))
        return std::nullopt;
    return static_cast<Skill>(av - 6);
}

} // namespace fp
