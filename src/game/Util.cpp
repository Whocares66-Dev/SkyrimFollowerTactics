#include "game/Util.h"

#include "core/Clock.h"

#include <mutex>

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

} // namespace ft::game
