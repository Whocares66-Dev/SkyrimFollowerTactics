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
// Read by the AI's score, on its own threads.
std::atomic_bool g_variedAiChoices{true};
std::atomic_bool g_selfDamageSpells{true};

} // namespace

ft::Settings CurrentSettings()
{
    ft::Settings settings;
    settings.requireDualWieldStyle = g_requireDualWieldStyle.load(std::memory_order_relaxed);
    settings.requireDualCastPerks = g_requireDualCastPerks.load(std::memory_order_relaxed);
    settings.requirePowerBashPerk = g_requirePowerBashPerk.load(std::memory_order_relaxed);
    settings.variedAiChoices = g_variedAiChoices.load(std::memory_order_relaxed);
    settings.selfDamageSpells = g_selfDamageSpells.load(std::memory_order_relaxed);
    return settings;
}

void SetSettings(const ft::Settings &settings)
{
    const ft::Settings before = CurrentSettings();
    g_requireDualWieldStyle.store(settings.requireDualWieldStyle, std::memory_order_relaxed);
    g_requireDualCastPerks.store(settings.requireDualCastPerks, std::memory_order_relaxed);
    g_requirePowerBashPerk.store(settings.requirePowerBashPerk, std::memory_order_relaxed);
    g_variedAiChoices.store(settings.variedAiChoices, std::memory_order_relaxed);
    g_selfDamageSpells.store(settings.selfDamageSpells, std::memory_order_relaxed);
    if (before.requireDualWieldStyle == settings.requireDualWieldStyle &&
        before.requireDualCastPerks == settings.requireDualCastPerks &&
        before.requirePowerBashPerk == settings.requirePowerBashPerk &&
        before.variedAiChoices == settings.variedAiChoices && before.selfDamageSpells == settings.selfDamageSpells)
        return;
    log::tactics.event(log::Level::Info, "settings.changed",
                       {{"requireDualWieldStyle", settings.requireDualWieldStyle},
                        {"requireDualCastPerks", settings.requireDualCastPerks},
                        {"requirePowerBashPerk", settings.requirePowerBashPerk},
                        {"variedAiChoices", settings.variedAiChoices},
                        {"selfDamageSpells", settings.selfDamageSpells}},
                       "settings: dual wield combat style {}, dual casting perks {}, power bash perk {}, "
                       "varied AI choices {}, self-targeting damage spells {}",
                       settings.requireDualWieldStyle ? "required" : "not required",
                       settings.requireDualCastPerks ? "required" : "not required",
                       settings.requirePowerBashPerk ? "required" : "not required",
                       settings.variedAiChoices ? "on" : "off", settings.selfDamageSpells ? "on" : "off");
}

} // namespace ft::game
