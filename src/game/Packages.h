#pragma once
// The UseMagic packages: how a rule makes a follower CAST a spell rather than
// merely hold it.
//
// THE MECHANISM, AND WHY IT IS THIS ONE
// Nothing in the game's API makes an NPC cast a chosen spell at a chosen
// moment (dev/MAGIC.md walks the seven ways that was established). The AI
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
//     load        the input layout is read off vanilla records (Calibrate)
//     first seen  the follower's own records are MADE IN MEMORY
//                 (game/Forms.h), each a copy of a vanilla instance with its
//                 own condition
//     rule fires  repoint their record's Spell input, put it at the FRONT of
//                 the follower's running package array (PutOnStack), point
//                 its condition at the follower, ask the AI to re-evaluate
//     the AI      finds the first entry whose condition passes -- ours --
//                 and runs the UseMagic procedure: animation, cost, interrupts
//     afterwards  the tick takes the record out of the array and clears the
//                 condition, so the follower's stack is exactly as it was
//
// (Until 2026-09-09 the record was spliced into the vanilla follower
// alias's combat-override list instead, which is shared and covered only
// the followers that alias holds; dev/MAGIC.md "The list they live in".)
//
// NO PLUGIN FILE, NOTHING IN THE SAVE
// Every record this needs is created in memory and forgotten at exit. The load
// order does not change, the save never references a form of ours (every
// lease is released on the save message, before the engine writes), and
// removing the DLL removes the mod. dev/MAGIC.md "Forms at runtime" has what
// was read from the executable to establish that.
//
// WHAT IS MADE
// For each follower: a UseMagic package (a copy of Mercer's cast-at-player
// record); a Shout package (a copy of Tsun's Clear Skies record), which is how
// a POWER is performed -- the UseMagic procedure never fires one
// (dev/ACTIONS.md 7); a one-word wrapper shout and its word; and a UseWeapon
// package (a copy of Edorfin's attack-a-target record), which is how a POWER
// ATTACK is made (dev/ATTACK.md). Each package's condition is
// GetIsReference(holder).
//
// ONE SET PER FOLLOWER
// A follower's records are made the first time the tick sees them and kept,
// by reference ID, until the game quits: forms are never deleted, so one who
// leaves keeps theirs, and one who rejoins -- or turns up in another save --
// gets them back. Every input in a record -- spell, target, cast time --
// belongs to that follower alone, and one cast at a time: a request while
// they hold a record reports their cast rules busy for that turn, no cooldown
// spent, and the next rule in the list gets its turn.
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
#include <string_view>
#include <vector>

namespace RE
{
class Actor;
} // namespace RE

namespace ft::game
{

struct BlowPlan;

// The spell a fresh record is pointed at, and the check that the layout
// found on vanilla records holds on the copies. Fast Healing.
inline constexpr std::uint32_t kCanarySpellID = 0x0002F3B8;

// Find the input layout on vanilla records. If any step fails casting
// reports unavailable and cast rules stay unsupported; the log says which
// step.
void InitPackages();

// Make a follower's records if they have none: a UseMagic package for a
// spell, and a Shout package with its wrapper for a power or a shout. The
// tick calls it for every follower before they are evaluated. A follower
// whose set could not be made is not tried again this launch; the log says
// why. Game thread.
void ProvideCastForms(RE::Actor *actor);

// Can this follower be made to cast: the layout was found at load and their
// records were made?
[[nodiscard]] bool HasCastForms(const RE::Actor *actor);

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
    NoPackages,     // no layout at load, or their records could not be made (see the log)
    AlreadyCasting, // this follower is mid-cast; one cast at a time
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
// ruleIndex, ruleName: the rule asking, named again in rule.resolved when the
// lease ends.
[[nodiscard]] CastRequest RequestCast(RE::Actor *actor, std::uint32_t spellFormID, std::uint32_t targetId,
                                      float sustainSeconds, bool dualCast, int ruleIndex, std::string_view ruleName);

// Ask a follower to use a power or a shout. A power (a spell record of type
// Power or Lesser Power): points their wrapper shout's first word at the
// power, makes the power a Voice spell for the lease, gives the follower the
// wrapper (the Shout procedure only fires a shout the actor has), and arms
// their Shout package as RequestCast does. A shout (a TESShout the follower
// has): the same package with the shout itself in its Shout input. targetId
// as for RequestCast. The lease ends on the voice's fire event for our
// shout, or at the deadline.
[[nodiscard]] CastRequest RequestShout(RE::Actor *actor, std::uint32_t formID, std::uint32_t targetId, int ruleIndex,
                                       std::string_view ruleName);

// Ask a follower for one power attack at an enemy, through their UseWeapon
// record: power attacks only, one attack, damage done, the location near
// themself, the rule's target. The procedure draws the attack from the race's
// power attacks, waits for the follower's own swing to end, and retries until
// the graph takes it (dev/ATTACK.md). The lease ends once the swing has
// ended, when the AI drops the package, or at the deadline. NoPackages when
// the follower has no such record: the checks at load failed, or the copy did.
// `plan` is the blow as the sensors priced it; its cost and reach go into
// rule.resolved beside the follower's stamina and distance, so a power attack
// not made says whether they could pay and reach.
[[nodiscard]] CastRequest RequestPowerAttack(RE::Actor *actor, std::uint32_t targetId, const BlowPlan &plan,
                                             int ruleIndex, std::string_view ruleName);

// Is a power attack's record held by anyone? Any thread: the pacing thread
// asks it to decide whether the fast tick is wanted.
[[nodiscard]] bool AnyWeaponLease() noexcept;

// TickPackages for the power attack records alone, for the fast tick: the
// record goes back as soon as the swing has ended, before the procedure can
// start a second one. Game thread.
void TickWeaponLeases(double now);

// Called every tick from the game thread. Watches held records: reports when
// the AI picks our package up, and releases the record once the cast has
// run or the window has passed. A record is never held longer than the
// window while the tick runs; that is the backstop that keeps a follower
// from standing held.
//
// `followers` is everyone under management. Any of them carrying a wrapper
// shout while casting nothing has one left by a lease that never ended; it
// is taken back here.
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
