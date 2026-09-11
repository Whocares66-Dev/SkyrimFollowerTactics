#include "game/Util.h"

#include <mutex>

namespace ft::game
{

namespace
{
std::mutex g_clockMutex;  // the hit sinks read the clock off the engine's threads
double g_lastHour = -1.0; // hour-of-day at the previous read, or -1
double g_seconds = 0.0;   // accumulated real seconds of game time
} // namespace

double TacticsSeconds()
{
    auto *calendar = RE::Calendar::GetSingleton();
    if (!calendar || !calendar->gameHour)
        return NowSeconds(); // before the game is up; nothing is timed then anyway

    std::scoped_lock lock(g_clockMutex);
    const double hour = calendar->gameHour->value;
    if (g_lastHour >= 0.0)
    {
        double deltaHours = hour - g_lastHour;
        if (deltaHours < 0.0)
            deltaHours += 24.0; // crossed midnight
        const double timescale = calendar->GetTimescale() > 0.0f ? calendar->GetTimescale() : 20.0;
        g_seconds += deltaHours * 3600.0 / timescale;
    }
    g_lastHour = hour;
    return g_seconds;
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
