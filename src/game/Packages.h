#pragma once
// The UseMagic package pool, and the one unmapped structure standing between us
// and rule-driven casting.
//
// WHY THIS EXISTS
// Nothing in the game's API makes an NPC cast a spell on demand. Verified, not
// assumed: CastSpellImmediate never animates an actor (the CK wiki says so and
// DynamicAnimationCasting uses exactly that call), MRh_SpellFire_Event is an
// event the game EMITS rather than accepts (NPC Spell Variance hooks it to
// detect casts), and there is no Actor.Cast in Papyrus at all. The sanctioned
// route is an AI package carrying the UseMagic procedure -- which is content,
// not code, hence FollowerTactics.esp.
//
// WHAT THE PLUGIN HOLDS
// Eight copies of a vanilla UseMagic package, FT_CastSlot1..8, local FormIDs
// 0x800..0x807. Eight because the spell lives IN the record: one shared record
// would mean two followers casting different spells overwrite each other, and
// eight matches the follower cap the engine already enforces.
//
// THE GAP
// Repointing a package's Spell input means writing through
// BGSPackageDataTargetSelector, whose layout CommonLibSSE does not map. This
// header exposes a PROBE rather than a setter, because writing through a
// guessed layout is how you corrupt a live game, and guessed layouts are
// exactly what has already cost this project several rounds.
//
// The probe is decisive because the plugin ships a CANARY: we wrote Fast
// Healing (0x0002F3B8) into that slot ourselves, so the correct interpretation
// is the one that reads that exact form back. Nothing is written until it does.

#include <cstdint>

namespace RE
{
class Actor;
class TESPackage;
} // namespace RE

namespace ft::game
{

// Local FormIDs of the pool, as authored by tools/xEdit. Verified by parsing
// the plugin rather than assumed from creation order.
inline constexpr std::uint32_t kFirstPackageLocalID = 0x000800;
inline constexpr std::size_t kPackageSlots = 8;
inline constexpr const char *kPluginName = "FollowerTactics.esp";

// The spell the plugin ships in every slot's Spell input. Its only job is to be
// a value we already know, so a memory probe has something to be right about.
inline constexpr std::uint32_t kCanarySpellID = 0x0002F3B8; // Fast Healing

// Resolve the pool. Safe to call when the plugin is absent -- everything simply
// reports unavailable and the cast action stays unsupported, which is the
// correct behaviour for a mod whose ESL was not enabled.
void InitPackages();

[[nodiscard]] bool PackagesAvailable();

// Dump what a package's Spell input actually looks like in memory, and say
// whether the canary was found. Logs only; writes nothing.
void ProbeSpellInput();

// Ask a follower to cast, by pushing her slot's UseMagic package.
//
// Which spell is NOT chosen here yet -- every slot ships casting the canary,
// and repointing that field needs the layout the probe is still establishing.
// So this refuses any other spell rather than casting the wrong one silently,
// which is the failure the whole exercise has been trying to avoid.
//
// Slots are assigned per actor and kept, because the spell lives in the record:
// two followers sharing a slot would overwrite each other's spell the moment
// repointing works.
enum class CastRequest : std::uint8_t
{
    Pushed,        // the package is on her; the game decides the rest
    NoPackages,    // the ESL is not enabled
    NoSlot,        // more followers than slots
    SpellNotInSlot // asked for a spell the package does not hold
};

[[nodiscard]] const char *ToString(CastRequest r) noexcept;

[[nodiscard]] CastRequest RequestCast(RE::Actor *actor, std::uint32_t spellFormID);

} // namespace ft::game
