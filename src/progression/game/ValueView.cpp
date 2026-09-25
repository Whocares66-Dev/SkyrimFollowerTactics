#include "progression/game/ValueView.h"

#include "game/Addresses.h"
#include "progression/core/Companion.h"
#include "progression/game/FastIds.h"
#include "progression/game/Log.h"

#include <atomic>
#include <memory>
#include <vector>

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
// Game thread: the actors the last Publish named, so one no longer named
// has its values marked stale too.
std::vector<RE::FormID> g_named;
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

// Every skill and attribute of these actors marked stale in their process's
// cache, as the engine's own setter does after a write: read next, each is
// worked out again through the base, and so through the view. Without it a
// cached value -- the skills' current values are cached -- keeps the number
// from before (seen 2026-09-22: the Skills tab's levels unmoved by a point
// moved). Their armour sum too, which the armour skills' levels go into
// and the engine keeps until told its inputs changed, as a perk's rank
// landing tells it (progression/game/PerkView.cpp); the view answers at
// once, so at once.
void MarkStale(const std::vector<RE::FormID> &ids)
{
    using MarkFn = void (*)(RE::Actor *, RE::ActorValue);
    static REL::Relocation<MarkFn> mark{addr::kMarkValueStale};
    for (const RE::FormID id : ids)
        if (auto *actor = RE::TESForm::LookupByID<RE::Actor>(id); actor && actor->Is3DLoaded())
        {
            for (int value = kFirstSkill; value < kFirstAttribute + static_cast<int>(kAttributeCount); ++value)
                mark(actor, static_cast<RE::ActorValue>(value));
            actor->OnArmorActorValueChanged();
        }
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
    std::vector<RE::FormID> named;
    named.reserve(bonuses.size());
    for (const auto &entry : bonuses)
        named.push_back(entry.first);
    g_skillCap.store(skillCap, std::memory_order_relaxed);
    g_fast.Set(bonuses);
    g_views.store(std::make_shared<const Views>(std::move(bonuses)), std::memory_order_release);
    // Those named now and those named before: joining, changing, leaving.
    std::vector<RE::FormID> touched = g_named;
    touched.insert(touched.end(), named.begin(), named.end());
    g_named = std::move(named);
    MarkStale(touched);
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
    g_named.clear();
    g_fast.Clear();
    g_views.store(std::make_shared<const Views>(), std::memory_order_release);
}

} // namespace fp::game::valueview
