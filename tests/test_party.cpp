// Who is who around an actor: the party, the enemies and the corpses the
// snapshot carries, from the facts the walk reads of each loaded actor.

#include <catch2/catch_test_macros.hpp>

#include "core/Party.h"

#include <vector>

using namespace ft;

namespace
{

constexpr ActorId kPlayer = 0x14;
constexpr ActorId kLydia = 0xA2C94;
constexpr ActorId kJenassa = 0x1348A;
constexpr ActorId kBandit = 0x101;
constexpr ActorId kWolf = 0x102;

ActorSeen Teammate(ActorId id)
{
    ActorSeen seen;
    seen.id = id;
    seen.teammate = true;
    return seen;
}

ActorSeen Hostile(ActorId id, bool inCombat = true)
{
    ActorSeen seen;
    seen.id = id;
    seen.inCombat = inCombat;
    seen.hostile = true;
    return seen;
}

ActorSeen Corpse(ActorId id, float distance)
{
    ActorSeen seen;
    seen.id = id;
    seen.dead = true;
    seen.distance = distance;
    seen.level = 12;
    return seen;
}

} // namespace

TEST_CASE("the player first among the allies, then the teammates in the walk's order; self never", "[party]")
{
    const std::vector<ActorSeen> loaded{Teammate(kJenassa), Teammate(kLydia), Hostile(kBandit)};
    const PartyPlan plan = AssembleParty(kLydia, kPlayer, true, loaded, 0);
    REQUIRE(plan.allies == std::vector<ActorId>{kPlayer, kJenassa});
    REQUIRE(plan.enemies == std::vector<ActorId>{kBandit});
    REQUIRE(plan.corpses.empty());

    // Built for the player: Self is the player, and the party is the
    // followers.
    const PartyPlan mine = AssembleParty(kPlayer, kPlayer, true, loaded, 0);
    REQUIRE(mine.allies == std::vector<ActorId>{kJenassa, kLydia});

    // No player, or the player dead: no player among the allies, and
    // nobody is an enemy by the compass.
    REQUIRE(AssembleParty(kLydia, 0, false, loaded, 0).allies == std::vector<ActorId>{kJenassa});
    REQUIRE(AssembleParty(kLydia, 0, false, loaded, 0).enemies.empty());
    REQUIRE(AssembleParty(kLydia, kPlayer, false, loaded, 0).allies == std::vector<ActorId>{kJenassa});
}

TEST_CASE("an enemy is in a fight and hostile to the player; a teammate is an ally whatever else", "[party]")
{
    ActorSeen fightingFriend = Teammate(kJenassa);
    fightingFriend.inCombat = true;
    fightingFriend.hostile = true; // a brawl, say: the flag wins
    const std::vector<ActorSeen> loaded{fightingFriend, Hostile(kBandit), Hostile(kWolf, false)};
    const PartyPlan plan = AssembleParty(kLydia, kPlayer, true, loaded, 0);
    REQUIRE(plan.allies == std::vector<ActorId>{kPlayer, kJenassa});
    // The wolf is hostile but not in a fight: not painted red yet.
    REQUIRE(plan.enemies == std::vector<ActorId>{kBandit});

    // The dead are on no side.
    ActorSeen deadBandit = Hostile(kBandit);
    deadBandit.dead = true;
    deadBandit.distance = 100.0f;
    const PartyPlan after = AssembleParty(kLydia, kPlayer, true, std::vector<ActorSeen>{deadBandit}, 0);
    REQUIRE(after.enemies.empty());
    REQUIRE(after.corpses.size() == 1);
}

TEST_CASE("the follower's own target is an enemy, once, whether or not the player is in its fight", "[party]")
{
    const std::vector<ActorSeen> loaded{Hostile(kBandit), Hostile(kWolf, false)};
    // The wolf is the target though the compass has not painted it.
    PartyPlan plan = AssembleParty(kLydia, kPlayer, true, loaded, kWolf);
    REQUIRE(plan.enemies == std::vector<ActorId>{kBandit, kWolf});
    // The bandit is the target and already an enemy: not twice.
    plan = AssembleParty(kLydia, kPlayer, true, loaded, kBandit);
    REQUIRE(plan.enemies == std::vector<ActorId>{kBandit});
    // No target: nothing added.
    REQUIRE(AssembleParty(kLydia, kPlayer, true, loaded, 0).enemies == std::vector<ActorId>{kBandit});
}

TEST_CASE("a corpse is the dead within reach, not commanded, not refused by the effect", "[party]")
{
    ActorSeen raised = Corpse(0x201, 500.0f);
    raised.commanded = true;
    ActorSeen refused = Corpse(0x202, 500.0f);
    refused.noReanimate = true;
    const std::vector<ActorSeen> loaded{Corpse(0x203, 3000.0f), Corpse(0x204, 3000.01f), raised, refused,
                                        Corpse(0x205, 10.0f)};
    const PartyPlan plan = AssembleParty(kLydia, kPlayer, true, loaded, 0);
    REQUIRE(plan.corpses.size() == 2);
    REQUIRE(plan.corpses[0].id == 0x203);
    REQUIRE(plan.corpses[0].level == 12);
    REQUIRE(plan.corpses[0].distance == 3000.0f);
    REQUIRE(plan.corpses[1].id == 0x205);
    REQUIRE(plan.allies == std::vector<ActorId>{kPlayer});
}
