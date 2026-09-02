#pragma once
// Sensors: turn a live RE::Actor into an ft::Snapshot.
//
// This is the boundary. Everything above it (ft::Snapshot, ft::Evaluate) is
// RE::-free and unit tested; everything below is imperative Skyrim code that
// can only be verified by playing. Keep this file thin and obvious.

#include "core/Snapshot.h"

#include <string>
#include <vector>

namespace RE
{
class Actor;
class AlchemyItem;
class SpellItem;
} // namespace RE

namespace ft::game
{

// The RE:: pointers an action may need, carried alongside the Snapshot rather
// than inside it -- ft::Snapshot must never see an RE:: type, and an action
// still has to be handed the actual potion to equip.
struct PotionChoice
{
    RE::AlchemyItem *health{nullptr};
    RE::AlchemyItem *magicka{nullptr};
    RE::AlchemyItem *stamina{nullptr};
};

// One castable spell a follower knows, for the editor's menu.
//
// Name and id together because the menu shows one and stores the other: the
// name is what a player picks by and is translated, the FormID is what the
// rule keeps and what survives a language change. Same split as wire names
// versus display names, for the same reason.
struct SpellOption
{
    std::uint32_t form{0};
    std::string name;
};

// One drinkable potion a follower carries, for the editor's menu.
struct PotionOption
{
    std::uint32_t form{0};
    std::string name;
    int count{0};
};

// Every drinkable potion she carries, sorted by name. Menu content only.
[[nodiscard]] std::vector<PotionOption> ScanCarriedPotions(RE::Actor *actor);

// Every spell the follower can actually cast, sorted by name.
//
// Sorted here rather than in the UI because the order is a property of the
// list, not of how it is drawn, and doing it once per rebuild beats doing it
// every frame the menu is open.
//
// Filtered to SpellType::kSpell. Abilities, diseases and passive effects also
// live in an actor's spell list and none of them are castable, so offering
// them would be offering rules that can never work.
[[nodiscard]] std::vector<SpellOption> ScanCastableSpells(RE::Actor *actor);

// Resolve a FormID from a rule back to the spell it names, or nullptr.
[[nodiscard]] RE::SpellItem *FindSpell(std::uint32_t form);

// Dump the actor's active magic effects to the log: source item, archetype,
// elapsed/duration, magnitude.
//
// This is here to answer one question empirically rather than from memory --
// does drinking a vanilla healing potion leave anything running that we could
// check? If it does, "is the effect I applied still active" is a far more
// precise availability test than a fixed cooldown, and it generalises to
// spells and food. If the list is empty, the effect is instant, the condition
// itself is the check, and MinimumCooldown stays the right mechanism.
//
// ActiveEffect carries `spell` (the AlchemyItem for a potion), `duration` and
// `elapsedSeconds`, so the check is exact once we know it is worth making.
void LogActiveEffects(RE::Actor *actor, const char *when);

// Build the snapshot for one follower. `now` is monotonic seconds since plugin
// load; cooldowns are measured against it.
//
// Phase 1 scope: this fills self/player state and the potion inventory only.
// The enemies and allies vectors are deliberately left EMPTY -- the marquee
// rule is Self + HealthPctBelow, which needs none of it, and every extra sensor
// is per-tick cost that has to be justified (docs/PLAN.md 3.3). Group subjects
// will not match until those are populated.
ft::Snapshot BuildSnapshot(RE::Actor *actor, double now, PotionChoice &choice);

} // namespace ft::game
