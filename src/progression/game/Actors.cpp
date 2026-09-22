#include "progression/game/Actors.h"

#include "progression/game/Forms.h"
#include "progression/game/Log.h"

#include <algorithm>
#include <limits>

namespace fp::game
{
namespace
{

constexpr RE::FormID kDismissedFollowerFaction = 0x0005C84C;

RE::ActorValue AV(Skill s)
{
    return static_cast<RE::ActorValue>(ActorValueOf(s));
}

RE::ActorValue AV(Attribute a)
{
    return static_cast<RE::ActorValue>(ActorValueOf(a));
}

} // namespace

bool IsPerson(RE::Actor *actor)
{
    if (!actor)
        return false;
    static RE::BGSKeyword *npc = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ActorTypeNPC");
    static RE::BGSKeyword *animal = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ActorTypeAnimal");
    static RE::BGSKeyword *creature = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ActorTypeCreature");
    const auto has = [&](RE::BGSKeyword *k) { return k && actor->HasKeyword(k); };
    if (has(npc))
        return true;
    return !has(animal) && !has(creature);
}

bool IsFollower(RE::Actor *actor)
{
    if (!actor || actor->IsPlayerRef() || actor->IsDead() || !actor->IsPlayerTeammate() || !IsPerson(actor))
        return false;
    const auto *dismissed = RE::TESForm::LookupByID<RE::TESFaction>(kDismissedFollowerFaction);
    return !(dismissed && actor->IsInFaction(dismissed));
}

bool IsWaiting(RE::Actor *actor)
{
    auto *owner = actor ? actor->AsActorValueOwner() : nullptr;
    return owner && owner->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.0f;
}

bool IsUniqueNpc(RE::Actor *actor)
{
    const auto *base = actor ? actor->GetActorBase() : nullptr;
    return base && base->IsUnique();
}

std::vector<RE::Actor *> LoadedFollowers()
{
    std::vector<RE::Actor *> out;
    auto *lists = RE::ProcessLists::GetSingleton();
    if (!lists)
        return out;
    for (auto &handle : lists->highActorHandles)
    {
        auto ptr = handle.get();
        RE::Actor *actor = ptr ? ptr.get() : nullptr;
        if (IsFollower(actor))
            out.push_back(actor);
    }
    return out;
}

PerSkill<int> BaseSkills(RE::Actor *actor)
{
    PerSkill<int> out{};
    auto *owner = actor ? actor->AsActorValueOwner() : nullptr;
    if (!owner)
        return out;
    for (const Skill s : AllSkills())
        out[Index(s)] = static_cast<int>(owner->GetBaseActorValue(AV(s)));
    return out;
}

PerAttribute<int> BaseAttributes(RE::Actor *actor)
{
    PerAttribute<int> out{};
    auto *owner = actor ? actor->AsActorValueOwner() : nullptr;
    if (!owner)
        return out;
    for (std::size_t i = 0; i < kAttributeCount; ++i)
        out[i] = static_cast<int>(owner->GetBaseActorValue(AV(static_cast<Attribute>(i))));
    return out;
}

void ApplyPoints(RE::Actor *actor, const Delta &delta)
{
    auto *owner = actor ? actor->AsActorValueOwner() : nullptr;
    if (!owner)
        return;
    for (const Skill s : AllSkills())
        if (const int d = delta.skills[Index(s)]; d != 0)
            owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, AV(s), static_cast<float>(d));
    for (std::size_t i = 0; i < kAttributeCount; ++i)
        if (const int d = delta.attributes[i]; d != 0)
            owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, AV(static_cast<Attribute>(i)),
                                 static_cast<float>(d));
}

std::unordered_set<FormKey, FormKeyHash> BasePerks(RE::Actor *actor)
{
    std::unordered_set<FormKey, FormKeyHash> out;
    auto *base = actor ? actor->GetActorBase() : nullptr;
    if (!base || !base->perks)
        return out;
    for (std::uint32_t i = 0; i < base->perkCount; ++i)
        if (auto *perk = base->perks[i].perk)
            if (auto key = KeyOf(perk))
                out.insert(std::move(*key));
    return out;
}

void DropPerkAbilities(RE::Actor *actor, RE::BGSPerk *perk)
{
    if (!actor || !perk)
        return;
    auto *base = actor->GetActorBase();
    auto *own = base ? base->GetSpellList() : nullptr;
    const auto onRecord = [&](RE::SpellItem *spell) {
        return own && own->spells &&
               std::find(own->spells, own->spells + own->numSpells, spell) != own->spells + own->numSpells;
    };
    for (RE::BGSPerkEntry *entry : perk->perkEntries)
        if (entry && entry->GetType() == RE::PERK_ENTRY_TYPE::kAbility)
            if (RE::SpellItem *spell = static_cast<RE::BGSAbilityPerkEntry *>(entry)->ability;
                spell && !onRecord(spell))
                if (actor->RemoveSpell(spell))
                    log::perks.debug("{}: {}'s ability {} taken off", NameOf(actor), NameOf(perk), NameOf(spell));
}

float DistanceToPlayer(RE::Actor *actor)
{
    auto *player = RE::PlayerCharacter::GetSingleton();
    if (!actor || !player)
        return std::numeric_limits<float>::max();
    return actor->GetPosition().GetDistance(player->GetPosition());
}

} // namespace fp::game
