#include "Effects.h"

#include "core/Names.h"

#include <algorithm>
#include <array>

namespace ft
{
namespace
{

struct Known
{
    std::string_view name;
    int group;
};

// The vanilla potion effects, in the order the menu lists them. Skills
// that do nothing in a fight -- haggling, lockpicking, pickpocketing,
// speech, smithing, enchanting, alchemy -- and carry weight and
// waterbreathing are listed last of the known, so a follower's bag of them
// does not push the useful ones down.
constexpr std::array<Known, 38> kPotionEffects{{
    {"Restore Health", 0},      {"Restore Stamina", 0},    {"Restore Magicka", 0},      {"Fortify Health", 1},
    {"Fortify Stamina", 1},     {"Fortify Magicka", 1},    {"Regenerate Health", 2},    {"Regenerate Stamina", 2},
    {"Regenerate Magicka", 2},  {"Resist Fire", 3},        {"Resist Frost", 3},         {"Resist Shock", 3},
    {"Resist Magic", 3},        {"Resist Poison", 3},      {"Fortify One-handed", 4},   {"Fortify Two-handed", 4},
    {"Fortify Archery", 4},     {"Fortify Marksman", 4},   {"Fortify Block", 4},        {"Fortify Heavy Armor", 4},
    {"Fortify Light Armor", 4}, {"Fortify Sneak", 4},      {"Fortify Alteration", 5},   {"Fortify Conjuration", 5},
    {"Fortify Destruction", 5}, {"Fortify Illusion", 5},   {"Fortify Restoration", 5},  {"Invisibility", 6},
    {"Cure Poison", 6},         {"Waterbreathing", 7},     {"Fortify Carry Weight", 7}, {"Fortify Barter", 7},
    {"Fortify Lockpicking", 7}, {"Fortify Pickpocket", 7}, {"Fortify Persuasion", 7},   {"Fortify Speech", 7},
    {"Fortify Smithing", 7},    {"Fortify Enchanting", 7},
}};

// The vanilla poison effects: the three damages; what damages over time;
// what stops recovering; the weaknesses; and what takes the fight out of
// them.
constexpr std::array<Known, 23> kPoisonEffects{{
    {"Damage Health", 0},
    {"Damage Stamina", 0},
    {"Damage Magicka", 0},
    {"Ravage Health", 1},
    {"Ravage Stamina", 1},
    {"Ravage Magicka", 1},
    {"Lingering Damage Health", 1},
    {"Lingering Damage Stamina", 1},
    {"Lingering Damage Magicka", 1},
    {"Damage Health Regen", 2},
    {"Damage Stamina Regen", 2},
    {"Damage Magicka Regen", 2},
    {"Weakness to Fire", 3},
    {"Weakness to Frost", 3},
    {"Weakness to Shock", 3},
    {"Weakness to Magic", 3},
    {"Weakness to Poison", 3},
    {"Fear", 4},
    {"Frenzy", 4},
    {"Paralysis", 4},
    {"Slow", 4},
    {"Frostbite Venom", 5},
    {"Mystic Venom", 5},
}};

// Where a name falls: its index in the table, or the table's size for one
// not in it. The index orders the known ones; the group is the table's.
template <std::size_t N> std::pair<std::size_t, int> Place(const std::array<Known, N> &table, std::string_view name)
{
    for (std::size_t i = 0; i < table.size(); ++i)
        if (table[i].name == name)
            return {i, table[i].group};
    return {table.size(), table.empty() ? 0 : table.back().group + 1};
}

} // namespace

std::vector<EffectEntry> ArrangeEffects(ConsumableKind kind, std::vector<std::string> names)
{
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    struct Ranked
    {
        std::string name;
        std::size_t index;
        int group;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(names.size());
    for (auto &name : names)
    {
        const auto [index, group] =
            kind == ConsumableKind::Poison ? Place(kPoisonEffects, name) : Place(kPotionEffects, name);
        ranked.push_back({std::move(name), index, group});
    }
    // The known by their place; the unknown after them, and among
    // themselves by name, which the sort above already gave and which
    // stable_sort keeps.
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked &a, const Ranked &b) { return a.index < b.index; });

    std::vector<EffectEntry> out;
    out.reserve(ranked.size());
    for (auto &r : ranked)
        out.push_back({std::move(r.name), r.group});
    return out;
}

bool EffectUseless(std::string_view effect) noexcept
{
    return effect == "Cure Disease" || effect == "Resist Disease";
}

std::string_view EffectLabel(std::string_view effect) noexcept
{
    for (const std::string_view prefix : {"Restore ", "Damage "})
    {
        if (effect.size() > prefix.size() && effect.substr(0, prefix.size()) == prefix)
        {
            const auto rest = effect.substr(prefix.size());
            if (rest == "Health" || rest == "Stamina" || rest == "Magicka")
                return rest;
        }
    }
    return effect;
}

} // namespace ft

namespace ft
{

bool EffectApplies(const EffectShape &shape, bool readsSkillMods, bool readsSkillPowerMods) noexcept
{
    if (!shape.valueModifier)
        return true;
    if (shape.skillModifier)
        return readsSkillMods;
    if (shape.skillPower)
        return readsSkillPowerMods;
    return true;
}

bool IsBuff(const EffectShape &shape, bool readsSkillMods, bool readsSkillPowerMods) noexcept
{
    return shape.duration > 0.0f && !shape.harmful && shape.peakValue && !shape.waterbreathing &&
           EffectApplies(shape, readsSkillMods, readsSkillPowerMods);
}

std::vector<PotionStock::Effect> ConsumableEffectsOf(std::span<const ConsumableEffectSeen> effects, ConsumableKind kind,
                                                     bool readsSkillMods, bool readsSkillPowerMods)
{
    std::vector<PotionStock::Effect> out;
    const bool firstOnly = kind == ConsumableKind::Ingredient;
    for (const ConsumableEffectSeen &effect : effects)
    {
        if (!effect.name.empty() && !EffectUseless(effect.name))
            out.push_back({effect.name, effect.magnitude, effect.duration,
                           IsBuff(effect.shape, readsSkillMods, readsSkillPowerMods), effect.shape.harmful});
        if (firstOnly)
            break;
    }
    return out;
}

std::vector<EffectPick> ArrangeEffectPicks(std::vector<EffectPick> candidates)
{
    std::vector<EffectPick> picks;
    for (EffectPick &candidate : candidates)
    {
        if (candidate.effect == 0 || candidate.name.empty() ||
            std::ranges::any_of(picks, [&](const EffectPick &pick) { return pick.name == candidate.name; }))
            continue;
        picks.push_back(std::move(candidate));
    }
    SortByName(picks, [](const auto &item) -> std::string_view { return item.name; });
    return picks;
}

} // namespace ft