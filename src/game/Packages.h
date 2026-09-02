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
// priority. The fix is not a higher-priority quest: it is that the follower is
// IN COMBAT, and an actor in combat does not run her package stack at all. She
// runs her alias's COMBAT OVERRIDE package list, which the vanilla follower
// alias already has (PlayerFollowerCombatOverridePackageList, 0005C852). The
// game's own worked example is Mercer Frey, whose "cast Nightingale Strife at
// the player" UseMagic package sits in exactly such a list, gated by a
// condition on quest stage. Ours are gated by a faction rank instead, because a
// faction rank is something this plugin can set in one call.
//
// So the bridge from a rule to a cast is:
//
//     load        our eight packages are inserted at the FRONT of the vanilla
//                 follower combat-override list
//     rule fires  repoint the slot's Spell input, set the follower's rank in
//                 FT_CastNow to her slot number, ask the AI to re-evaluate
//     the AI      finds the first list entry whose condition passes -- ours --
//                 and runs the UseMagic procedure: animation, cost, interrupts
//     afterwards  the tick clears the rank, so the condition fails again and
//                 the list falls through to the vanilla entries exactly as
//                 before
//
// WHAT THE PLUGIN HOLDS
// Eight UseMagic packages FT_CastSlot1..8 (0x800..0x807) and the faction
// FT_CastNow (0x808) with ranks 0..15. Slot k's condition is
// GetFactionRank(FT_CastNow) == k.
//
// THE POOL
// The eight records are a resource pool. A follower takes a free record when
// a cast rule fires, the record is HERS ALONE until the cast has run (or the
// window has passed), and then it goes back. Never shared, even for the same
// spell: every input in the record -- spell, target, cast time -- belongs to
// the holder. The limit is eight followers mid-cast at the same instant. When
// that is exceeded, or she already holds one, the rule engine reports her cast
// rules busy for that turn -- no cooldown is spent, and the next rule in the
// list gets its turn.
//
// THE INPUTS, AND HOW THEY ARE FOUND
// Three inputs are written per request: Spell, Target (Self for herself, a
// specific reference for anyone else), and for a concentration spell the two
// CastTime floats that say how long the stream runs. The container that holds
// a package's inputs is not mapped by CommonLibSSE, so none of these offsets
// is hard-coded: CalibrateInputs finds each one at load by looking for the
// value the record was authored with (Fast Healing, Self, 0.5 / 1.0), and
// nothing is written through a layout that did not read back as expected.
//
// LIMITS, STATED
// - Only a follower the vanilla DialogueFollower alias holds is covered: the
//   override list belongs to that alias. A follower recruited by a framework
//   (NFF, EFF, AFT) runs its own alias. RequestCast reports which case she is.
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

inline constexpr std::uint32_t kFirstPackageLocalID = 0x000800;
inline constexpr std::uint32_t kCastFactionLocalID = 0x000808;
inline constexpr std::size_t kPackageSlots = 8;
inline constexpr const char *kPluginName = "FollowerTactics.esp";

// The quest that owns the vanilla follower alias.
inline constexpr std::uint32_t kDialogueFollowerQuestID = 0x000750BA;

// The combat-override lists our packages are spliced into, front of each. A
// plugin that is not loaded is skipped. Only the vanilla follower list today;
// docs/MAGIC.md "Follower frameworks" records what SFF and NFF use (SFF the
// same vanilla list, NFF its own nwsFollowerCombatPkList 007429), for when
// integrating with them is on the table.
struct OverrideList
{
    const char *plugin;
    std::uint32_t localID;
};
inline constexpr OverrideList kOverrideLists[] = {
    {"Skyrim.esm", 0x0005C852}, // PlayerFollowerCombatOverridePackageList
};

// The spell every slot ships with, so the memory probe has a known value to
// find. Fast Healing.
inline constexpr std::uint32_t kCanarySpellID = 0x0002F3B8;

// Resolve the pool and splice it into the follower combat-override list. Safe
// when the plugin is absent: everything reports unavailable and cast rules stay
// unsupported, which is the right behaviour for a mod whose ESL is unticked.
void InitPackages();

[[nodiscard]] bool PackagesAvailable();

// Could a cast be started right now? False while every slot is mid-cast, so
// the rule engine can skip cast rules for this evaluation instead of firing
// one that cannot be honoured.
[[nodiscard]] bool HasFreeSlot();

// Is this follower holding a record right now? The rule engine treats her
// cast rules as busy while she is, so a second request during a cast is
// skipped for that turn without spending a cooldown.
[[nodiscard]] bool IsMidCast(const RE::Actor *actor);

// Locate the Spell, Target and CastTime inputs in memory by their canaries.
// Writes nothing; what it fails to find, RequestCast refuses to write.
void CalibrateInputs();

enum class CastRequest : std::uint8_t
{
    Armed,          // her slot's condition now passes; the AI decides the rest
    NoPackages,     // the ESL is not enabled
    PoolBusy,       // every record is held by a follower mid-cast
    AlreadyCasting, // this follower already holds a record; one cast at a time
    SpellNotInSlot, // the Spell input could not be repointed at that spell
    TargetGone      // the rule's target no longer resolves to a loaded actor
};

[[nodiscard]] const char *ToString(CastRequest r) noexcept;

// Ask a follower to cast a spell. targetId is the rule's resolved target: her
// own id (or zero) casts on herself; any other actor is written into the
// record's Target input for the duration of the lease, so an offensive spell
// goes at the enemy she is engaging and a heal can go to the player.
// sustainSeconds applies to a CONCENTRATION spell (Flames, vanilla Healing):
// how long to hold the stream. Zero means the default. Ignored for a
// fire-and-forget spell.
[[nodiscard]] CastRequest RequestCast(RE::Actor *actor, std::uint32_t spellFormID, std::uint32_t targetId,
                                      float sustainSeconds);

// Called every tick from the game thread. Watches held slots: reports when
// the AI picks our package up, and releases the record -- rank back to -1 --
// once the cast has run or the window has passed. A record is never held
// longer than the window while the tick runs; that is the backstop that keeps
// the pool from draining.
//
// `followers` is everyone under management. Any of them carrying a rank while
// holding no record has a STALE rank -- typically loaded from a save made
// mid-cast -- and would otherwise pass her slot's condition forever. It is
// cleared here, so the faction can never wedge a follower into casting on
// every evaluation.
void TickPackages(double now, const std::vector<RE::Actor *> &followers);

// Forget every held record. For a game load: the handles are meaningless in
// the new session and the ranks, if any survived in the save, are swept by the
// first tick.
void ResetPackages();

} // namespace ft::game
