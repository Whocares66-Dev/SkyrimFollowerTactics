#include "game/Settings.h"

#include "game/Log.h"

#include <atomic>

namespace ft::game
{
namespace
{

// Atomics rather than a lock: read on every snapshot, written by a click
// and by a load. The defaults are vanilla's own answer -- the combat style
// decides who dual wields, and no perk is asked of a follower.
std::atomic_bool g_requireDualWieldStyle{true};
std::atomic_bool g_requireDualCastPerks{false};
std::atomic_bool g_requirePowerBashPerk{false};

} // namespace

ft::Settings CurrentSettings()
{
    ft::Settings settings;
    settings.requireDualWieldStyle = g_requireDualWieldStyle.load(std::memory_order_relaxed);
    settings.requireDualCastPerks = g_requireDualCastPerks.load(std::memory_order_relaxed);
    settings.requirePowerBashPerk = g_requirePowerBashPerk.load(std::memory_order_relaxed);
    return settings;
}

void SetSettings(const ft::Settings &settings)
{
    const ft::Settings before = CurrentSettings();
    g_requireDualWieldStyle.store(settings.requireDualWieldStyle, std::memory_order_relaxed);
    g_requireDualCastPerks.store(settings.requireDualCastPerks, std::memory_order_relaxed);
    g_requirePowerBashPerk.store(settings.requirePowerBashPerk, std::memory_order_relaxed);
    if (before.requireDualWieldStyle == settings.requireDualWieldStyle &&
        before.requireDualCastPerks == settings.requireDualCastPerks &&
        before.requirePowerBashPerk == settings.requirePowerBashPerk)
        return;
    log::tactics.event(log::Level::Info, "settings.changed",
                       {{"requireDualWieldStyle", settings.requireDualWieldStyle},
                        {"requireDualCastPerks", settings.requireDualCastPerks},
                        {"requirePowerBashPerk", settings.requirePowerBashPerk}},
                       "settings: dual wield combat style {}, dual casting perks {}, power bash perk {}",
                       settings.requireDualWieldStyle ? "required" : "not required",
                       settings.requireDualCastPerks ? "required" : "not required",
                       settings.requirePowerBashPerk ? "required" : "not required");
}

} // namespace ft::game
