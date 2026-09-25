#include "game/Util.h"

#include "core/Clock.h"
#include "game/Log.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>

namespace ft::game
{

namespace
{
std::mutex g_clockMutex; // the hit sinks read the clock off the engine's threads
ft::GameClock g_clock;
} // namespace

double TacticsSeconds()
{
    auto *calendar = RE::Calendar::GetSingleton();
    if (!calendar || !calendar->gameHour)
        return NowSeconds(); // before the game is up; nothing is timed then anyway

    std::scoped_lock lock(g_clockMutex);
    return g_clock.Sample(calendar->gameHour->value, calendar->GetTimescale());
}

std::string Describe(RE::Actor *actor)
{
    if (!actor)
        return "<null actor>";

    const char *name = actor->GetDisplayFullName();
    return fmt::format("{} ({:08X})", (name && *name) ? name : "<unnamed>", actor->GetFormID());
}

std::string DisplayNameOf(RE::Actor *actor)
{
    if (!actor)
        return "<unknown>";
    const char *name = actor->GetDisplayFullName();
    return (name && *name) ? name : "<unnamed>";
}

namespace
{

// --- character sheet ---------------------------------------------------------

// The setting of that name in the game's collection, asked on every use and
// never kept: a mod that changes a parameter mid-session (an MCM slider)
// changes the sheet with it, and the lookup is a hash of the name. A missing
// name is said once, at warn, since a misspelt one would otherwise be a
// vanilla number that looks right. CommonLib's "name"_gs literal is the same
// lookup but keeps the setting in a static and says nothing of a miss.
template <class T> const RE::Setting *FindSetting(const char *name, T vanilla)
{
    auto *collection = RE::GameSettingCollection::GetSingleton();
    if (const auto *setting = collection ? collection->GetSetting(name) : nullptr)
        return setting;
    static std::unordered_set<std::string> missing;
    if (missing.insert(name).second)
        log::sensors.warn("game setting {} not found -- using vanilla's {}", name, vanilla);
    return nullptr;
}

} // namespace

// A float game setting ("f" names), or the vanilla value where the collection
// has none. The fallbacks are vanilla's numbers so a missing setting degrades
// to what the unmodded game does, not to a zero that reads as a broken sheet.
float GameSetting(const char *name, float vanilla)
{
    const auto *setting = FindSetting(name, vanilla);
    return setting ? setting->GetFloat() : vanilla;
}

// An integer game setting ("i" names), the same way.
std::int32_t GameSetting(const char *name, std::int32_t vanilla)
{
    const auto *setting = FindSetting(name, vanilla);
    return setting ? setting->GetInteger() : vanilla;
}

} // namespace ft::game
