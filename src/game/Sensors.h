#pragma once
// Sensors: turn a live RE::Actor into an ft::Snapshot.
//
// This is the boundary. Everything above it (ft::Snapshot, ft::Evaluate) is
// RE::-free and unit tested; everything below is imperative Skyrim code that
// can only be verified by playing. Keep this file thin and obvious.

#include "core/Snapshot.h"

namespace RE
{
class Actor;
class AlchemyItem;
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
