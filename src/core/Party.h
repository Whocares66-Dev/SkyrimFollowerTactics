#pragma once
// Who is who around an actor, by definition (dev/CONDITIONS.md 6): the
// party and the enemies the snapshot carries, and the corpses a Reanimate
// could take. The game walks the loaded actors and reads a few facts of
// each (game/Sensors.cpp, BuildSnapshot); which side each is on, in what
// order, and whether the follower's own target is added, is decided here,
// where it is tested. No Skyrim.

#include "Snapshot.h"

#include <span>
#include <vector>

namespace ft
{

// What the walk reads of one loaded actor.
struct ActorSeen
{
    ActorId id{0};
    bool dead{false};
    bool teammate{false}; // the player-teammate flag
    bool inCombat{false};
    bool hostile{false};     // to the player: what the compass paints red
    bool commanded{false};   // a raised corpse is someone's
    bool noReanimate{false}; // the MagicNoReanimate keyword, the Reanimate archetype's one condition
    float distance{0.0f};    // from the actor the snapshot is for
    int level{0};
};

// The party, the enemies and the corpses, as ids in the order the
// snapshot carries them: the player first among the allies (alive, and
// not the actor themself), then the loaded actors in the walk's order,
// each an ally by the teammate flag or an enemy by being in a fight and
// hostile to the player; the follower's own target last among the
// enemies when it is not one already, whether or not the player is in its
// fight yet. The corpses are the dead within reach, not commanded, not
// refused by the effect's own condition.
struct PartyPlan
{
    std::vector<ActorId> allies;
    std::vector<ActorId> enemies;
    std::vector<CorpseView> corpses;
};

inline constexpr float kCorpseReach = 3000.0f;

// `player` is the player's id, or 0 with no player; `playerAlive` whether
// they are. `loaded` is every loaded actor but the player, the actor
// themself included (skipped by id).
[[nodiscard]] PartyPlan AssembleParty(ActorId self, ActorId player, bool playerAlive, std::span<const ActorSeen> loaded,
                                      ActorId currentTarget);

} // namespace ft
