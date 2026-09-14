#pragma once
// The UseMagic package pool: how a rule makes a follower CAST a spell rather
// than merely hold it.
//
// THE MECHANISM, AND WHY IT IS THIS ONE
// Nothing in the game's API makes an NPC cast a chosen spell at a chosen
// moment (docs/MAGIC.md walks the seven ways that was established). The AI
// casts when one of ITS packages says to, so the only honest route is to give
// the AI a package and a reason to pick it.
//
// The first attempt pushed the package straight onto the actor and lost on
// priority. What works is the follower's OWN package stack: every alias an
// actor fills is instanced for them as an array of packages on the actor,
// and the array whose package is running now is the stack that has them in
// the fight. The game's own worked example is Mercer Frey, whose "cast
// Nightingale Strife at the player" UseMagic package is gated by a
// condition on quest stage. Ours are gated by `GetIsReference(<holder>)`: a
// condition whose parameter is a pointer this plugin writes.
//
// So the bridge from a rule to a cast is:
//
//     load        sixteen packages are MADE IN MEMORY (game/Forms.h), each
//                 a copy of a vanilla instance with its own condition
//     rule fires  repoint the slot's Spell input, put the record at the
//                 FRONT of the follower's running package array
//                 (PutOnStack), point the slot's condition at the follower,
//                 ask the AI to re-evaluate
//     the AI      finds the first entry whose condition passes -- ours --
//                 and runs the UseMagic procedure: animation, cost, interrupts
//     afterwards  the tick takes the record out of the array and clears the
//                 condition, so the follower's stack is exactly as it was
//
// (Until 2026-09-09 the record was spliced into the vanilla follower
// alias's combat-override list instead, which is shared and covered only
// the followers that alias holds; docs/MAGIC.md "The list they live in".)
//
// NO PLUGIN FILE, NOTHING IN THE SAVE
// Every record this needs is created at load and forgotten at exit. The load
// order does not change, the save never references a form of ours (every
// lease is released on the save message, before the engine writes), and
// removing the DLL removes the mod. docs/MAGIC.md "Forms at runtime" has what
// was read from the executable to establish that.
//
// WHAT IS MADE
// Eight UseMagic packages (copies of Mercer's cast-at-player record), eight
// Shout packages (copies of Tsun's Clear Skies record), eight one-word wrapper
// shouts and their words. Slot k's condition is GetIsReference(holder of k).
//
// THE POOL
// The sixteen records are a resource pool. A follower takes a free record when
// a cast rule fires, the record is HERS ALONE until the cast has run (or the
// window has passed), and then it goes back. Never shared, even for the same
// spell: every input in the record -- spell, target, cast time -- belongs to
// the holder. The limit is eight followers mid-cast at the same instant. When
// that is exceeded, or they already hold one, the rule engine reports their cast
// rules busy for that turn -- no cooldown is spent, and the next rule in the
// list gets its turn.
//
// THE INPUTS, AND HOW THEY ARE FOUND
// Three inputs are written per request: Spell, Target (Self for themself, a
// specific reference for anyone else), and for a concentration spell the two
// CastTime floats that say how long the stream runs. The container that holds
// a package's inputs is not mapped by CommonLibSSE, so none of these offsets
// is hard-coded: the layout is found at load by looking for values vanilla
// records were authored with (Mercer's spell, the player as his target, his
// 0.5 / 1.0 cast time; Colette's Target = Self), and nothing is written
// through a layout that did not read back as expected. The copies are then
// checked the same way: Fast Healing is written into each and read back.
//
// LIMITS, STATED
// - A follower whose aliases instance no package array on them -- none is
//   running -- has no stack to put the record on; RequestCast says so.
// - A concentration spell's fire event marks the START of the stream, so it
//   is not a release signal for one; the stream is released on the CastStop
//   that follows, or when the target dies, or at a deadline.

#include <cstdint>
#include <vector>

namespace RE
{
class Actor;
} // namespace RE

namespace ft::game
{

// The pool is sixteen package records. Slots 0..7 are UseMagic and cast a
// spell from a hand; slots 8..15 are Shout and cast from the voice, which is
// how a POWER is performed -- the UseMagic procedure never fires one
// (docs/ACTIONS.md 7). Each Shout slot's record points at its own wrapper
// shout, a one-word shout whose word's spell is repointed at the rule's
// power for the lease.
inline constexpr std::size_t kSpellSlots = 8;
inline constexpr std::size_t kVoiceSlots = 8;
inline constexpr std::size_t kPackageSlots = kSpellSlots + kVoiceSlots;

// The spell every slot is pointed at once made, and the check that the
// layout found on vanilla records holds on the copies. Fast Healing.
inline constexpr std::uint32_t kCanarySpellID = 0x0002F3B8;

// Find the input layout on vanilla records and make the pool. If any step
// fails everything reports unavailable and cast rules stay unsupported; the
// log says which step.
void InitPackages();

[[nodiscard]] bool PackagesAvailable();

// Could a cast be started right now? False while every spell slot is
// mid-cast, so the rule engine can skip cast rules for this evaluation
// instead of firing one that cannot be honoured. HasFreeVoiceSlot is the
// same for the shout slots a power goes through.
[[nodiscard]] bool HasFreeSlot();
[[nodiscard]] bool HasFreeVoiceSlot();

// Is this form one of our wrapper shouts? They sit in a follower's shout
// list only for the length of a lease, and the Magic tab leaves them out.
[[nodiscard]] bool IsWrapperShout(std::uint32_t formID);

// Is this power leased to a shout slot right now? For the lease its record
// reads as a Voice spell (RequestShout), and the menus that sort spells by
// type ask this so the power does not vanish from them meanwhile.
[[nodiscard]] bool IsLeasedPower(std::uint32_t formID);

// Is this follower holding a record right now? The rule engine treats
// their cast rules as busy while they are, so a second request during a
// cast is skipped for that turn without spending a cooldown.
[[nodiscard]] bool IsMidCast(const RE::Actor *actor);

// Is this form what a record leased to the follower casts right now: a
// cast slot's spell, a shout slot's shout, or the wrapper and the power
// behind it? The package's own equip of it, on its way to casting, is
// ours, and the equip detours let it through a ban or a pinned hand.
[[nodiscard]] bool IsOurCast(const RE::Actor *actor, std::uint32_t formID);

enum class CastRequest : std::uint8_t
{
    Armed,          // their slot's condition now passes; the AI decides the rest
    NoPackages,     // the pool could not be made at load (see the log)
    PoolBusy,       // every record is held by a follower mid-cast
    AlreadyCasting, // this follower already holds a record; one cast at a time
    SpellNotInSlot, // the Spell input could not be repointed at that spell
    TargetGone      // the rule's target no longer resolves to a loaded actor
};

[[nodiscard]] const char *ToString(CastRequest r) noexcept;

// Ask a follower to cast a spell. targetId is the rule's resolved target: their
// own id (or zero) casts on themself; any other actor is written into the
// record's Target input for the duration of the lease, so an offensive spell
// goes at the enemy they are engaging and a heal can go to the player.
// sustainSeconds applies to a CONCENTRATION spell (Flames, vanilla Healing):
// how long to hold the stream. Zero means the default. Ignored for a
// fire-and-forget spell.
// dualCast: from both hands, the procedure's DualCast input; the caller has
// judged that the follower can.
[[nodiscard]] CastRequest RequestCast(RE::Actor *actor, std::uint32_t spellFormID, std::uint32_t targetId,
                                      float sustainSeconds, bool dualCast);

// Ask a follower to use a power or a shout. A power (a spell record of type
// Power or Lesser Power): takes a free Shout slot, points its wrapper
// shout's first word at the power, makes the power a Voice spell for the
// lease, gives the follower the wrapper (the Shout procedure only fires a
// shout the actor has), and arms the slot as RequestCast does. A shout (a
// TESShout the follower has): the same slot with the shout itself in the
// package's Shout input. targetId as for RequestCast. The lease ends on the
// voice's fire event for our shout, or at the deadline.
[[nodiscard]] CastRequest RequestShout(RE::Actor *actor, std::uint32_t formID, std::uint32_t targetId);

// Called every tick from the game thread. Watches held slots: reports when
// the AI picks our package up, and releases the record once the cast has
// run or the window has passed. A record is never held longer than the
// window while the tick runs; that is the backstop that keeps the pool from
// draining.
//
// `followers` is everyone under management. Any of them carrying a wrapper
// shout while holding no record has one left by a lease that never ended
// (a crash mid-cast); it is taken back here.
void TickPackages(double now, const std::vector<RE::Actor *> &followers);

// Release every held record now. For the save message: a follower running
// one of our packages, carrying a wrapper, or shouting a re-typed power at
// the instant the engine writes would put that into the save, and nothing
// of ours belongs there. The cast in progress, if any, is abandoned.
void ReleaseAllLeases(const char *why);

// Forget every held record. For a game load: the handles are meaningless in
// the new session, and the conditions are cleared so no slot passes for an
// actor object that no longer exists.
void ResetPackages();

} // namespace ft::game
