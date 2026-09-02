#pragma once
// The in-game panel. Registered with SKSE Menu Framework if it is present, and
// silently skipped if it is not -- the mod does not depend on it.

namespace ft::game::ui
{

// Register the panel. Call once, after kDataLoaded.
void Install();

// The render callback. __stdcall because that is the calling convention the
// framework's function pointer type demands.
// Add a menu entry for any follower that does not have one yet. Called from the
// tick, because followers appear after Install() has already run.
void RegisterNewFollowers();

} // namespace ft::game::ui
