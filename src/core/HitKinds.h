#pragma once
// What a hit or an effect landing counts as, for the Hit by condition and
// the Attacker target: the engine's events are read into these facts
// (game/Hits.cpp) and the kinds they note are decided here, tested. No
// Skyrim.

#include "Kinds.h"

#include <cstdint>
#include <span>
#include <vector>

namespace ft
{

// What an effect resists, by its record: the kind it counts as, else
// Magic.
enum class Resist : std::uint8_t
{
    Other,
    Fire,
    Frost,
    Shock,
    Poison
};
[[nodiscard]] DamageKind KindOfResist(Resist resist) noexcept;

// A blow or a spell landing, as read from the hit event: a magic item's
// detrimental effects each by what they resist, whether the item is a
// poison, else a weapon, a fist or a trap, ranged when it arrived as a
// projectile.
struct HitSeen
{
    bool magic{false};
    std::vector<Resist> detrimental; // the magic item's harmful effects
    bool poison{false};
    bool projectile{false};
};

// The kinds the hit notes, in order: for a magic item, Magic and the
// effect's own kind for each detrimental effect, and Poison for a poison
// whatever its effects say; for anything else, Ranged or Melee.
[[nodiscard]] std::vector<DamageKind> KindsOfHit(const HitSeen &hit);

} // namespace ft
