#include "fix/DispelHold.h"

#include "game/Log.h"
#include "game/Sensors.h"
#include "game/Util.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace ft::fix
{
namespace
{

// Entries whose hold the log has said, once each until a load.
std::mutex g_mutex;
std::unordered_set<const void *> g_said;

// The effect running on them that casting this would put out, or null.
// Only a spell on themself touches their own effects.
const RE::EffectSetting *RunningEffectDispelledBy(RE::Actor *actor, RE::MagicItem *magic)
{
    if (magic->GetDelivery() != RE::MagicSystem::Delivery::kSelf)
        return nullptr;
    std::vector<const RE::BGSKeyword *> dispels;
    for (const auto *effect : game::ResolvedEffects(*magic))
        if (effect->baseEffect->data.flags.all(RE::EffectSetting::EffectSettingData::Flag::kDispelWithKeywords))
            for (const auto *keyword : effect->baseEffect->GetKeywords())
                if (keyword)
                    dispels.push_back(keyword);
    if (dispels.empty())
        return nullptr;
    const RE::EffectSetting *running = nullptr;
    game::ForEachActiveEffect(actor, [&](RE::ActiveEffect &active) {
        if (running || active.spell == magic)
            return;
        const auto *base = active.effect->baseEffect;
        if (std::ranges::any_of(dispels, [base](const RE::BGSKeyword *keyword) { return base->HasKeyword(keyword); }))
            running = base;
    });
    return running;
}

} // namespace

float HoldBackDispellers(RE::CombatInventoryItem *entry, RE::Actor *actor, float engine)
{
    auto *magic = entry && entry->item && actor && engine > 0.0f ? entry->item->As<RE::MagicItem>() : nullptr;
    if (!magic)
        return engine;
    const RE::EffectSetting *running = RunningEffectDispelledBy(actor, magic);
    if (!running)
        return engine;
    bool first = false;
    {
        std::scoped_lock lock(g_mutex);
        first = g_said.insert(entry).second;
    }
    if (first)
        log::fix.debug("{} AI's {} held back while {} runs: casting it would dispel that", game::Describe(actor),
                       log::NameOf(entry->item), log::NameOf(running));
    return 0.0f;
}

void ResetDispelHold()
{
    std::scoped_lock lock(g_mutex);
    g_said.clear();
}

} // namespace ft::fix
