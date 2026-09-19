#include "core/Party.h"

#include <algorithm>

namespace ft
{

PartyPlan AssembleParty(ActorId self, ActorId player, bool playerAlive, std::span<const ActorSeen> loaded,
                        ActorId currentTarget)
{
    PartyPlan plan;
    // Not the player among their own allies: built for the player, Self
    // is the player, and the party is the followers.
    if (player != 0 && playerAlive && player != self)
        plan.allies.push_back(player);
    for (const ActorSeen &other : loaded)
    {
        if (other.id == self || other.id == player)
            continue;
        if (!other.dead)
        {
            if (other.teammate)
                plan.allies.push_back(other.id);
            else if (player != 0 && other.inCombat && other.hostile)
                plan.enemies.push_back(other.id);
            continue;
        }
        // The dead nearby that a Reanimate could take.
        if (other.commanded || other.noReanimate || other.distance > kCorpseReach)
            continue;
        plan.corpses.push_back({other.id, other.level, other.distance});
    }
    // The follower's own target is an enemy whether or not the player is
    // in its fight yet.
    if (currentTarget != 0 && std::find(plan.enemies.begin(), plan.enemies.end(), currentTarget) == plan.enemies.end())
        plan.enemies.push_back(currentTarget);
    return plan;
}

} // namespace ft
