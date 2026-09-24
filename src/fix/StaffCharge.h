#pragma once
// The combat AI's staff charge threshold, priced as a use is priced.
//
// When the engine counts an actor's items for their combat inventory, a
// staff copy whose charge is below its enchantment's cost is counted as no
// copy, and the AI never picks the staff up (44884, and its copy inlined in
// the rebuild 44879; dev/VERSIONS.md has the sites). It asks that cost with NO caster,
// so without the perks a use is actually charged with: Serana's Rahgot's
// Staff cost 125 a use with Serana's perks and was ignored below 250
// (2026-09-23). Both calls are wrapped: whatever each called before -- the
// engine's cost function, or another mod's hook on the same call -- is
// called on with the actor whose inventory it is as the caster it left
// out. Every actor, not only followers: the engine is wrong for all of
// them.

namespace ft::fix
{

// Once, at data load. A site that is not a call instruction (another
// patch's shape) is left alone, and the log says so; a call already
// hooked by another mod is wrapped, and the log says that too.
void InstallStaffChargeFix();

} // namespace ft::fix
