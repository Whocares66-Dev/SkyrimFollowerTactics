#pragma once
// The Phase 1 spike: find followers, evaluate one hardcoded rule against each,
// and act on the result.
//
// Scope is deliberately narrow (docs/PLAN.md section 4, Phase 1):
//   if a follower's health drops below 50%, drink the best health potion,
//   at most once every 10 seconds.
//
// No UI, no JSON, no per-follower profiles. This exists to retire one risk --
// whether an NPC can be made to reliably consume a potion -- because if that
// cannot be done, the marquee rule does not work and the concept needs
// rethinking.

namespace ft::game
{

// Start ticking. Safe to call once, after kDataLoaded.
void Install();

// Enable/disable at runtime without unregistering the tick.
void SetEnabled(bool enabled);
[[nodiscard]] bool IsEnabled();

} // namespace ft::game
