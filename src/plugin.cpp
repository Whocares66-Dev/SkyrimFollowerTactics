// SKSE entry point: logging, the co-save, the messages, and at data load
// the packages, the tick, the panel and the hooks (src/game/). The rule
// engine is tested under Catch2 (tests/), and the plugin links the same
// library the tests do, so nothing is re-proved here.

#include "game/Hits.h"
#include "game/Log.h"
#include "game/Packages.h"
#include "game/Pins.h"
#include "game/Profiles.h"
#include "game/Tactics.h"
#include "game/UI.h"

namespace
{

void OnDataLoaded()
{
    // The cast packages' input layout, read off vanilla records: if any step
    // of that fails it reports unavailable and cast rules stay off, rather
    // than failing the whole plugin. Each follower's records are made when
    // the tick first sees them.
    ft::game::InitPackages();

    ft::game::Install();
    ft::game::ui::Install();
    ft::game::WatchCombatScores();
    ft::game::RefuseEquipsAgainstPins();
    ft::game::WatchHits();

    ft::log::plugin.event(ft::log::Level::Info, "plugin.loaded", {}, "FollowerTactics loaded");
}

} // namespace

SKSEPluginLoad(const SKSE::LoadInterface *skse)
{
    ft::log::Init();
    // NOT CommonLibSSE-NG's own logging: by default SKSE::Init opens the same
    // FollowerTactics.log with truncation, puts its own logger in as the
    // default -- at info in a release build, debug in a debug one, whatever
    // the ini says -- and writes a version banner. Every line then went
    // through its logger, the ini's level never applied to a release build,
    // and the lines our Init wrote were truncated away (2026-09-11).
    SKSE::Init(skse, {.log = false});

    ft::log::plugin.info("FollowerTactics starting up");

    // Tactics live in the co-save: registered here, before any save can
    // be loaded.
    ft::game::InstallSerialization();

    const bool listening =
        SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message *message) {
            if (message->type == SKSE::MessagingInterface::kDataLoaded)
                OnDataLoaded();

            // A load or a new game invalidates every handle a lease holds.
            // Drop the leases. (The rules, switches and pins are reset by
            // the serialization revert callback, which runs before the save's
            // records load.)
            if (message->type == SKSE::MessagingInterface::kPostLoadGame ||
                message->type == SKSE::MessagingInterface::kNewGame)
                ft::game::ResetPackages();

            // Sent before the engine writes the save (SKSE's SaveGame hook
            // dispatches it, then calls the original). A follower mid-cast is
            // running a package of ours, may carry a wrapper shout, may be
            // shouting a re-typed power: all of it would go into the file, and
            // the packages are runtime forms that the save cannot bring back
            // whole. Every lease ends here, so the save holds nothing of ours.
            if (message->type == SKSE::MessagingInterface::kSaveGame)
                ft::game::ReleaseAllLeases("saving");
        });
    if (!listening)
        ft::log::plugin.error("could not register for SKSE's messages -- nothing of the mod will start");

    return true;
}
