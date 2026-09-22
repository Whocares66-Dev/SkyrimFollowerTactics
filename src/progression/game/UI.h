#pragma once
// The panel, in SKSE Menu Framework's Mod Control Panel (F1), laid out as
// dev/PROGRESSION.md's "Interface" and styled as Follower Tactics' panel is:
//
//   Follower Tactics / Progression / Overview           everyone the save knows
//   Follower Tactics / Progression / Settings           the player's choices, tests
//   Follower Tactics / Progression / Companions / Lydia the companion's sheet
//
// Draws on the render thread from progression/game/Service.h's Snapshot; every change
// is an action queued to the game thread.

namespace fp::game::ui
{

// At data load. Without the framework installed there is no panel, and the
// rest of the mod runs.
void Install();

// Game thread, after each tick: an entry for each companion the ledger has
// that has none yet. An entry cannot be moved or (in the framework builds in
// the field) removed once added, so one is added once and kept.
void SyncEntries();

} // namespace fp::game::ui
