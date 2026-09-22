#include "progression/game/Learning.h"

#include "progression/game/Addresses.h"
#include "progression/game/FastIds.h"
#include "progression/game/Log.h"
#include "progression/game/Rules.h"
#include "progression/game/Service.h"

#include <atomic>
#include <random>
#include <unordered_map>

namespace fp::game::learning
{
namespace
{

FastIds g_ids;
bool g_installed = false;
std::atomic<std::uint64_t> g_magic{0};
std::atomic<std::uint64_t> g_blows{0};
std::atomic<std::uint64_t> g_struck{0};
// Which game a queued use belongs to: moved on by Forget.
std::atomic<std::uint32_t> g_session{0};

using UseSkillFn = void (*)(RE::Actor *, RE::ActorValue, float, RE::TESForm *, std::uint32_t);
using VictimFn = void (*)(RE::Actor *, RE::HitData &);
UseSkillFn g_useSkill = nullptr;
VictimFn g_victim = nullptr;

bool Counted(const RE::Actor *actor)
{
    return actor && !actor->IsPlayerRef() && g_ids.Maybe(actor->GetFormID());
}

// Off whatever thread the engine is on, onto the game thread, where the
// ledger lives.
void Queue(const RE::Actor *actor, RE::ActorValue av, float points)
{
    const auto skill = SkillFromActorValue(static_cast<int>(av));
    auto *tasks = SKSE::GetTaskInterface();
    if (!skill || points <= 0.0f || !tasks)
        return;
    const std::uint32_t session = g_session.load(std::memory_order_relaxed);
    tasks->AddTask([id = actor->GetFormID(), s = *skill, points, session] {
        if (session == g_session.load(std::memory_order_relaxed))
            OnSkillUse(id, s, points);
    });
}

// Character::UseSkill, slot 0xF7. The original is a `ret`; it is called
// anyway, in case another mod has put something there.
void UseSkillHook(RE::Actor *self, RE::ActorValue av, float points, RE::TESForm *form, std::uint32_t usage)
{
    if (Counted(self))
    {
        g_magic.fetch_add(1, std::memory_order_relaxed);
        Queue(self, av, points);
    }
    g_useSkill(self, av, points, form, usage);
}

RE::TESObjectARMO *EquippedShield(RE::Actor *actor)
{
    using Fn = RE::TESObjectARMO *(*)(RE::Actor *);
    static REL::Relocation<Fn> shield{addr::kEquippedShield};
    return shield(actor);
}

// The attacker's side, as 38627 works it out for the player (its block
// guarded by `aggressor == player`): a blow that landed, for damage, on a
// living grown-up. The skill is the one the hit was made with; the points,
// a bash's from the engine's own formula, otherwise the weapon's base
// damage.
void Attacker(RE::Actor *aggressor, RE::Actor *victim, const RE::HitData &hit)
{
    if (victim->IsDead(false) || victim->IsChild() || hit.totalDamage <= 0.0f)
        return;
    float points = 0.0f;
    if (hit.flags.any(RE::HitData::Flag::kBash))
    {
        using Fn = float (*)(float, bool);
        static REL::Relocation<Fn> bash{addr::kBashSkillUse};
        points = bash(hit.totalDamage, EquippedShield(aggressor) != nullptr);
    }
    else if (hit.weapon)
        points = static_cast<float>(hit.weapon->GetAttackDamage());
    g_blows.fetch_add(1, std::memory_order_relaxed);
    Queue(aggressor, hit.skill, points);
}

// The victim's side, as 38627 works it out for the player (guarded by
// `victim == player`): a blocked blow trains Block by what the block took;
// any other trains Heavy or Light Armor by the physical damage, chosen at
// random among the worn armour -- one chance in six per piece, the body's
// counting twice and the shield's not at all, so a half-dressed fighter
// trains less. The base record's race says which slots are the shield and
// the body (15695 and 15696 ask it; SE's 37589 the same).
void Victim(RE::Actor *victim, const RE::HitData &hit)
{
    g_struck.fetch_add(1, std::memory_order_relaxed);
    if (hit.flags.any(RE::HitData::Flag::kBlocked))
    {
        const float points = (hit.physicalDamage - hit.totalDamage) * SettingFloat("fWeaponBlockSkillUseMult", 1.0f) +
                             SettingFloat("fWeaponBlockSkillUseBase", 0.0f);
        Queue(victim, RE::ActorValue::kBlock, points);
        return;
    }
    const auto &biped = victim->GetBiped1(false);
    if (!biped)
        return;
    const auto *base = victim->GetActorBase();
    const auto *race = base ? base->race : nullptr;
    const std::size_t shield =
        race ? static_cast<std::size_t>(race->data.shieldObject.get()) : RE::BIPED_OBJECTS::kShield;
    const std::size_t body = race ? static_cast<std::size_t>(race->data.bodyObject.get()) : RE::BIPED_OBJECTS::kBody;
    int light = 0;
    int heavy = 0;
    for (std::size_t slot = 0; slot < RE::BIPED_OBJECTS::kTotal; ++slot)
    {
        if (slot == shield)
            continue;
        auto *armor = biped->objects[slot].item ? biped->objects[slot].item->As<RE::TESObjectARMO>() : nullptr;
        if (!armor || armor->GetArmorType() == RE::BIPED_MODEL::ArmorType::kClothing ||
            (armor->armorRating & 0xFFFF) == 0)
            continue;
        const int weight = slot == body ? 2 : 1;
        (armor->GetArmorType() == RE::BIPED_MODEL::ArmorType::kHeavyArmor ? heavy : light) += weight;
    }
    thread_local std::minstd_rand dice{std::random_device{}()};
    const int roll = static_cast<int>(dice() % 6);
    if (roll < light)
        Queue(victim, RE::ActorValue::kLightArmor, hit.physicalDamage);
    else if (roll < light + heavy)
        Queue(victim, RE::ActorValue::kHeavyArmor, hit.physicalDamage);
}

void VictimHook(RE::Actor *victim, RE::HitData &hit)
{
    if (victim)
    {
        const auto aggressor = hit.aggressor.get();
        if (Counted(aggressor.get()))
            Attacker(aggressor.get(), victim, hit);
        if (Counted(victim))
            Victim(victim, hit);
    }
    g_victim(victim, hit);
}

} // namespace

void Install()
{
    if (REL::Module::IsVR())
    {
        log::growth.warn("Skyrim VR: the skill-use hooks are not installed; companions learn nothing by doing");
        return;
    }
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_Character[0]};
    g_useSkill = reinterpret_cast<UseSkillFn>(
        *reinterpret_cast<std::uintptr_t *>(table.address() + addr::kUseSkillSlot * sizeof(std::uintptr_t)));
    table.write_vfunc(addr::kUseSkillSlot, &UseSkillHook);

    // The hit handler's call is only rewritten while it is still a plain
    // call (E8): another mod may have changed it, and what write_call hands
    // back as the original would then be nothing to call.
    const REL::Relocation<std::uintptr_t> site{addr::kHitHandler, addr::kHitHandlerVictimCall};
    if (*reinterpret_cast<const std::uint8_t *>(site.address()) != 0xE8 || SKSE::GetTrampoline().empty())
    {
        log::growth.error("the hit handler's call at {:X} is not a plain call, or there is no trampoline: companions "
                          "learn from magic only",
                          site.address());
        g_installed = true;
        return;
    }
    g_victim = reinterpret_cast<VictimFn>(SKSE::GetTrampoline().write_call<5>(site.address(), &VictimHook));
    g_installed = true;
    log::growth.info("Character's UseSkill replaced and the hit handler's victim call hooked: companions learn by "
                     "doing");
}

bool Installed() noexcept
{
    return g_installed;
}

Counters Count() noexcept
{
    return {g_magic.load(std::memory_order_relaxed), g_blows.load(std::memory_order_relaxed),
            g_struck.load(std::memory_order_relaxed)};
}

void Forget()
{
    g_ids.Clear();
    g_session.fetch_add(1, std::memory_order_relaxed);
}

void Publish(const std::vector<RE::FormID> &actors)
{
    std::unordered_map<RE::FormID, bool> map;
    for (const RE::FormID id : actors)
        map.emplace(id, true);
    g_ids.Set(map);
}

} // namespace fp::game::learning
