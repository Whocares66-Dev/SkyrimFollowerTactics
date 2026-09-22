#pragma once
// The eighteen skills and three attributes, in the engine's own order: a
// skill's index plus 6 is its actor value (One-Handed is 6, Enchanting 23),
// an attribute's plus 24 (Health 24, Magicka 25, Stamina 26). The game side
// converts with that offset and nothing else (progression/game/Actors.cpp).
//
// Which of them a companion is trained in is decided here: the six combat
// skills, the five schools and Sneak. A companion never smiths, brews,
// enchants, barters, picks a lock or a pocket, so training those would be
// growth nobody sees.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fp
{

enum class Skill : std::uint8_t
{
    OneHanded,
    TwoHanded,
    Archery,
    Block,
    Smithing,
    HeavyArmor,
    LightArmor,
    Pickpocket,
    Lockpicking,
    Sneak,
    Alchemy,
    Speech,
    Alteration,
    Conjuration,
    Destruction,
    Illusion,
    Restoration,
    Enchanting,
};

inline constexpr std::size_t kSkillCount = 18;

enum class Attribute : std::uint8_t
{
    Health,
    Magicka,
    Stamina,
};

inline constexpr std::size_t kAttributeCount = 3;

// A value per skill, indexed by the enum.
template <typename T> using PerSkill = std::array<T, kSkillCount>;
template <typename T> using PerAttribute = std::array<T, kAttributeCount>;

[[nodiscard]] constexpr std::size_t Index(Skill s) noexcept
{
    return static_cast<std::size_t>(s);
}
[[nodiscard]] constexpr std::size_t Index(Attribute a) noexcept
{
    return static_cast<std::size_t>(a);
}

// Every skill, in order.
[[nodiscard]] const std::array<Skill, kSkillCount> &AllSkills() noexcept;

// What the player reads: "One-Handed", "Heavy Armor".
[[nodiscard]] std::string_view Name(Skill s) noexcept;
[[nodiscard]] std::string_view Name(Attribute a) noexcept;

// What a file reads: "OneHanded", "HeavyArmor". Stable; saves use it.
[[nodiscard]] std::string_view Key(Skill s) noexcept;
[[nodiscard]] std::optional<Skill> SkillFromKey(std::string_view key) noexcept;
[[nodiscard]] std::string_view Key(Attribute a) noexcept;
[[nodiscard]] std::optional<Attribute> AttributeFromKey(std::string_view key) noexcept;

// A school of magic: the five whose spells a tome can teach.
[[nodiscard]] bool IsSchool(Skill s) noexcept;

// The engine's actor value numbers, for the game side and for the files
// that name them.
[[nodiscard]] constexpr int ActorValueOf(Skill s) noexcept
{
    return static_cast<int>(s) + 6;
}
[[nodiscard]] constexpr int ActorValueOf(Attribute a) noexcept
{
    return static_cast<int>(a) + 24;
}
[[nodiscard]] std::optional<Skill> SkillFromActorValue(int av) noexcept;

} // namespace fp
