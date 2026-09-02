#pragma once
// Steering the combat AI's own casting decision, instead of casting for it.
//
// WHY THIS AND NOT THE PACKAGE
// The UseMagic package route works exactly as documented and still failed: the
// package was pushed and the actor's own package kept running --
//     current package 000B9987 -> 000B9987 (not ours -- outranked)
// -- because package priority comes from the source, and an actor's own package
// outranks one merely pushed onto her. Fixing that needs a QUST with reference
// aliases at priority 99, which is a large record and an unverified amount of
// C++ (alias filling has no obvious native entry point).
//
// This is smaller and sidesteps priority entirely: the combat AI already asks
// itself "should I start a restore cast now?" through a virtual function. We
// answer it. Everything else -- animation, magicka, charge time, interruption,
// targeting -- stays the game's, because we never take over the cast.
//
// WHAT IS CERTAIN AND WHAT IS NOT
//   certain    CombatMagicCasterRestore::CheckStartCast is vfunc 06, its vtable
//              comes from Address Library (VariantID 265007/211142), and
//              CombatController::cachedAttacker gives the deciding actor.
//   NOT known  whether writing CombatMagicCaster::magicItem actually changes
//              WHICH spell gets cast, or whether the game re-derives it from
//              the inventory item. That is the experiment.
//
// The logging is built to separate those, and to separate both from the thing
// that has confounded every test so far: a follower's AI heals itself anyway
// when badly hurt. Every decision records what the ORIGINAL would have
// answered, so "she healed" can be attributed rather than assumed.

#include <cstdint>

namespace RE
{
class Actor;
}

namespace ft::game
{

// Install the vtable hook. Safe to call once, after kDataLoaded.
void InstallCombatHook();

[[nodiscard]] bool CombatHookInstalled();

// Ask for a cast the next time the AI considers one for this actor.
//
// A request, not a command, and deliberately short-lived: the combat AI decides
// when to ask, so a request that is not taken up within a couple of seconds is
// stale and would fire at a moment the rule never intended.
void RequestCombatCast(RE::Actor *actor, std::uint32_t spellFormID);

} // namespace ft::game
