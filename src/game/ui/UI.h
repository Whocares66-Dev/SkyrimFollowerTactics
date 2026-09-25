#pragma once
// The in-game panel, drawn with ImGui via SKSE Menu Framework: the rule
// editor and the character sheet -- inventory, magic, effects, summons,
// character, skills -- of a follower and of the player. Registered with the
// framework if it is present, and silently skipped if it is not -- the mod
// does not depend on it. Why a rule did or did not act is not a column here:
// a verdict lasted one tick and blanked when the fight ended, so it goes to
// the events log (dev/EVENTS.md).
//
// Everything in game/ui runs on the render thread. It never touches an
// RE::Actor and never reaches into live engine state -- the Observe* calls
// hand back a published view, shared and never edited after
// (game/Tactics.h). Reading a follower's inventory from the render thread
// would be a good way to crash the game.

#include "core/Snapshot.h"

namespace ft::game::ui
{

// The panel's top tabs. Also what the game thread builds by: the page on
// screen is the only one anyone can be reading, so it is the only one built
// (game/Tactics.h, RefreshShownPage). CombatStyle is a follower's page
// alone; Tactics is everyone's.
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
    IdleTactics,
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
