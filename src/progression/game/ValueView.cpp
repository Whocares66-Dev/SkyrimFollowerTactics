#include "progression/game/ValueView.h"

#include "game/Addresses.h"
#include "progression/core/Companion.h"
#include "progression/game/FastIds.h"
#include "progression/game/Log.h"

#include <atomic>
#include <memory>

namespace fp::game::valueview
{
namespace
{

// The first skill's actor value (One-Handed) and the first attribute's
// (Health), as Skills.h numbers them.
constexpr int kFirstSkill = 6;
constexpr int kFirstAttribute = 24;

using Views = std::unordered_map<RE::FormID, Bonus>;

std::atomic<std::shared_ptr<const Views>> g_views;
std::atomic<int> g_skillCap{100};
FastIds g_fast;

using GetBaseFn = float (*)(RE::ActorValueOwner *, RE::ActorValue);
GetBaseFn g_getBase = nullptr;
bool g_installed = false;
std::ptrdiff_t g_ownerOffset = 0;

// Character's GetBaseActorValue: the engine's answer, and for a managed
// companion's skill or attribute, what they learned on top of it.
float GetBaseHook(RE::ActorValueOwner *self, RE::ActorValue av)
{
    const float base = g_getBase(self, av);
    const int value = static_cast<int>(av);
    if (value < kFirstSkill || value >= kFirstAttribute + static_cast<int>(kAttributeCount))
        return base;
    const auto *actor = reinterpret_cast<const RE::Actor *>(reinterpret_cast<std::uintptr_t>(self) - g_ownerOffset);
    const RE::FormID id = actor->GetFormID();
    if (!g_fast.Maybe(id))
        return base;
    const auto views = g_views.load(std::memory_order_acquire);
    const auto it = views->find(id);
    if (it == views->end())
        return base;
    if (value < kFirstAttribute)
        return WithLearned(base, it->second.skills[static_cast<std::size_t>(value - kFirstSkill)],
                           g_skillCap.load(std::memory_order_relaxed));
    return base + static_cast<float>(it->second.attributes[static_cast<std::size_t>(value - kFirstAttribute)]);
}

} // namespace

void Install()
{
    g_views.store(std::make_shared<const Views>());
    g_ownerOffset = addr::ActorValueOwnerOffset();
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_Character[addr::kCharacterValueOwnerTable]};
    g_getBase = reinterpret_cast<GetBaseFn>(
        *reinterpret_cast<std::uintptr_t *>(table.address() + addr::kGetBaseActorValueSlot * sizeof(std::uintptr_t)));
    table.write_vfunc(addr::kGetBaseActorValueSlot, &GetBaseHook);
    g_installed = true;
    log::growth.info("Character's GetBaseActorValue replaced: companions' skills and attributes are read with "
                     "what they learned, and never written");
}

bool Installed() noexcept
{
    return g_installed;
}

void Publish(std::unordered_map<RE::FormID, Bonus> bonuses, int skillCap)
{
    g_skillCap.store(skillCap, std::memory_order_relaxed);
    g_fast.Set(bonuses);
    g_views.store(std::make_shared<const Views>(std::move(bonuses)), std::memory_order_release);
}

float EngineBase(RE::Actor *actor, RE::ActorValue av)
{
    auto *owner = actor ? actor->AsActorValueOwner() : nullptr;
    if (!owner)
        return 0.0f;
    return g_installed ? g_getBase(owner, av) : owner->GetBaseActorValue(av);
}

void Forget()
{
    g_fast.Clear();
    g_views.store(std::make_shared<const Views>(), std::memory_order_release);
}

} // namespace fp::game::valueview
