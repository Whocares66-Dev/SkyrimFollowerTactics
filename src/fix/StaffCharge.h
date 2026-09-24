#pragma once
// The combat AI's staff charge threshold, priced as a use is priced.
//
// When the engine counts an actor's items for their combat inventory, a
// staff copy whose charge is below its enchantment's cost is counted as no
// copy, and the AI never picks the staff up (44884, and its copy inlined in
// the rebuild 44879; dev/VERSIONS.md has the sites). It asks that cost with NO caster,
// so without the perks a use is actually charged with: Serana's Rahgot's
// Staff cost 125 a use with Serana's perks and was ignored below 250
// (2026-09-23). Both calls are rewritten to ask it of the actor whose
// inventory it is -- the engine's own cost function, with the caster it
// left out. Every actor, not only followers: the engine is wrong for all
// of them.

namespace ft::fix
{

// Once, at data load. Each call is rewritten only while it is still the
// plain call read from the executable; either left alone is said in the
// log.
void InstallStaffChargeFix();

} // namespace ft::fix
