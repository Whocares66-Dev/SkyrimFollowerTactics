#pragma once
// The in-game panel. Registered with SKSE Menu Framework if it is present, and
// silently skipped if it is not -- the mod does not depend on it.

#include "core/Snapshot.h"

namespace ft::game::ui
{

// The panel's top tabs. Also what the game thread builds by: the page on
// screen is the only one anyone can be reading, so it is the only one built
// (game/Tactics.h, RefreshShownPage). CombatStyle and Tactics are a
// follower's page alone.
enum class Tab
{
    None,
    Character,
    Inventory,
    Magic,
    Shouts,
    Summons,
    Effects,
    Skills,
    CombatStyle,
    Tactics,
};

// Whose page is on screen and which tab of it; None for a closed panel, and
// before the first frame is drawn. Written by the render thread as it draws
// the body of a tab, read by the tick.
struct ShownPage
{
    ft::ActorId actor{0};
    Tab tab{Tab::None};
};
[[nodiscard]] ShownPage Shown();

// One word for a tab: the id of its region, and what the log calls it.
[[nodiscard]] const char *Name(Tab tab);

// Register the panel. Call once, after kDataLoaded.
void Install();

// Add a menu entry for any follower that does not have one yet, and remove
// the entry of anyone dismissed where the framework allows it. Called from
// the tick, because followers come and go after Install() has run.
void SyncFollowers();

} // namespace ft::game::ui
