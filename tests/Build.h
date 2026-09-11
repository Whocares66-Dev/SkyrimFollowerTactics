#pragma once
// What the tests build snapshots with, shared. No Skyrim here either.

#include "core/Snapshot.h"

namespace ft::test
{

// The player, as an ally of the snapshot: the one with the player's id,
// added at full everything and close by on the first ask. A Player
// condition reads the player from the allies, as the game builds them
// (docs/CONDITIONS.md 6).
inline ActorView &Player(Snapshot &s)
{
    for (auto &a : s.allies)
        if (a.id == kPlayerFormID)
            return a;
    s.allies.push_back({kPlayerFormID, {100.0f, 100.0f}, 100.0f});
    ActorView &player = s.allies.back();
    player.magicka = player.stamina = {100.0f, 100.0f};
    return player;
}

} // namespace ft::test
