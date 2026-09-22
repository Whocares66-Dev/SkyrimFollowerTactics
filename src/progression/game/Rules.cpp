#include "progression/game/Rules.h"

#include "game/Addresses.h"

namespace fp::game
{
namespace
{

RE::Setting *Find(const char *name)
{
    auto *settings = RE::GameSettingCollection::GetSingleton();
    return settings ? settings->GetSetting(name) : nullptr;
}

int SettingInt(const char *name, int fallback)
{
    const RE::Setting *s = Find(name);
    return s ? s->GetInteger() : fallback;
}

} // namespace

float SettingFloat(const char *name, float fallback)
{
    const RE::Setting *s = Find(name);
    return s ? s->GetFloat() : fallback;
}

Rules ReadRules()
{
    Rules r;
    r.skillUseCurve = SettingFloat("fSkillUseCurve", static_cast<float>(r.skillUseCurve));
    r.xpPerSkillRank = SettingFloat("fXPPerSkillRank", static_cast<float>(r.xpPerSkillRank));
    r.levelUpBase = SettingFloat("fXPLevelUpBase", static_cast<float>(r.levelUpBase));
    r.levelUpMult = SettingFloat("fXPLevelUpMult", static_cast<float>(r.levelUpMult));
    r.attributePerLevel = SettingInt("iAVDhmsLevelUp", r.attributePerLevel);
    r.skillStart = SettingInt("iAVDSkillStart", r.skillStart);
    return r;
}

std::optional<SkillUsage> ReadSkillUsage(Skill skill)
{
    using Usage = bool (*)(RE::ActorValue, float *, float *, float *, float *);
    static REL::Relocation<Usage> usage{addr::kSkillUsage};
    // The engine's defaults before it asks, as 41561 sets them.
    float useMult = 1.0f;
    float useOffset = 0.0f;
    float improveMult = 1.0f;
    float improveOffset = 0.0f;
    if (!usage(static_cast<RE::ActorValue>(ActorValueOf(skill)), &useMult, &useOffset, &improveMult, &improveOffset))
        return std::nullopt;
    return SkillUsage{useMult, useOffset, improveMult, improveOffset};
}

int RaceSkillBonus(RE::Actor *actor, Skill skill)
{
    auto *race = actor ? actor->GetRace() : nullptr;
    if (!race)
        return 0;
    int bonus = 0;
    for (const auto &boost : race->data.skillBoosts)
        if (static_cast<int>(boost.skill.get()) == ActorValueOf(skill))
            bonus += boost.bonus;
    return bonus;
}

PerAttribute<int> RaceStart(RE::Actor *actor)
{
    auto *race = actor ? actor->GetRace() : nullptr;
    if (!race)
        return {100, 100, 100};
    return {static_cast<int>(race->data.startingHealth), static_cast<int>(race->data.startingMagicka),
            static_cast<int>(race->data.startingStamina)};
}

} // namespace fp::game
