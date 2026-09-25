#pragma once
// Sensors: turn a live RE::Actor into an ft::Snapshot.
//
// This is the boundary. Everything above it (ft::Snapshot, ft::Evaluate) is
// RE::-free and unit tested; everything below is imperative Skyrim code that
// can only be verified by playing. Keep this file thin and obvious.

#include "core/BagView.h"
#include "core/Blows.h"
#include "core/Breakdown.h"
#include "core/Effects.h"
#include "core/Rule.h"
#include "core/Snapshot.h"
#include "core/Views.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RE
{
class Actor;
class AlchemyItem;
class BGSAttackData;
struct Effect;
class InventoryEntryData;
class MagicItem;
class SpellItem;
class TESObjectARMO;
class TESObjectWEAP;
} // namespace RE

namespace ft::game
{

// The equip slot records, by FormID in Skyrim.esm: read off the game, not
// from memory, which had them wrong. Not through the default object
// table, which did not answer for them on this game (01:29): a null slot
// handed to an equip means "the default", and the default is the right
// hand -- which is where every left-hand dagger went (01:51).
inline constexpr std::uint32_t kRightHandSlot = 0x00013F42;
inline constexpr std::uint32_t kLeftHandSlot = 0x00013F43;
inline constexpr std::uint32_t kEitherHandSlot = 0x00013F44;
inline constexpr std::uint32_t kBothHandsSlot = 0x00013F45;
inline constexpr std::uint32_t kVoiceSlot = 0x00025BEE;

// A stat as the bars and the rules read it: the current value, and the
// maximum with every modifier in -- the permanent (perks, race) and the
// temporary (a Fortify enchantment or potion).
[[nodiscard]] ft::Stat ReadStat(RE::Actor *actor, RE::ActorValue av);

// Build the snapshot for one follower. `now` is monotonic seconds since plugin
// load; cooldowns are measured against it. Self and player state, the
// potions and the loadout, and the party and the enemies by definition:
// allies are the player and the other teammates, enemies whoever is in
// combat and hostile to the player (dev/CONDITIONS.md 6).
//
// `priced` is the spells a rule names: those alone are priced -- their
// magicka cost, whether they dual cast and at what cost, a Reanimate's cap
// -- since only a Cast rule's own spell is ever read from the prices, and
// pricing every spell the player knows measured 19 ms of a 20 ms snapshot
// (2026-09-19, Nordic Souls: the engine's cost calculation walks the perk
// entry points per spell, twice for the dual cast). Every spell is still
// listed as known and in the loadout.
ft::Snapshot BuildSnapshot(RE::Actor *actor, double now, const std::vector<std::uint32_t> &priced);

// Where a snapshot's time goes, step by step -- self, party, hands,
// spells, effects, the bag -- each with its microseconds since last asked,
// in build order. On the tick's cost line, so a slow snapshot names its
// step. Game thread.
struct StepCost
{
    const char *name{""};
    double totalUs{0.0};
    double maxUs{0.0};
    std::uint64_t samples{0};
};
[[nodiscard]] std::vector<StepCost> TakeSnapshotCosts();

// Whether tactics are for this actor at all: a person, not a beast.
// Shadowmere is a player teammate, and so is a dog follower and a summoned
// familiar -- none of them has a bag worth managing, a spell to place or a
// hand to put a weapon in, and a page of tactics for a horse is a page
// nobody can use (reported in play, 2026-09-19).
//
// The test is negative -- an animal or a creature that is NOT also marked
// an NPC -- rather than a plain "must be an NPC", so a modded follower
// whose race carries no type keyword at all keeps their page. Game thread.
[[nodiscard]] bool IsPerson(RE::Actor *actor);

// The engine's own word for a follower who was one and is not:
// DismissedFollowerFaction, which the dismissal puts them in. A faction is
// on the actor wherever they are, so this answers for a follower in
// another hold as readily as for one standing here -- unlike the teammate
// flag, which some frameworks leave set after a dismissal and which would
// otherwise keep them on the panel. Game thread.
inline constexpr std::uint32_t kDismissedFollowerFaction = 0x0005C84C;
inline constexpr std::uint32_t kCurrentFollowerFaction = 0x0005C84E;
inline constexpr std::uint32_t kPotentialFollowerFaction = 0x0005C84D;
[[nodiscard]] bool IsDismissedFollower(RE::Actor *actor);

// Which of the follower marks an actor actually carries, for the log:
// "teammate=1 current=0 dismissed=0 potential=0". Written once per
// follower as the panel first lists them, because which mark a given
// follower mod maintains is not knowable from the records -- Inigo runs
// his own follow system and is in none of the vanilla factions, so the
// teammate flag is all there is to go on for him, and whether a framework
// keeps CurrentFollowerFaction up to date decides whether we could ever
// require it. Game thread.
[[nodiscard]] std::string FollowerMarks(RE::Actor *actor);
} // namespace ft::game
