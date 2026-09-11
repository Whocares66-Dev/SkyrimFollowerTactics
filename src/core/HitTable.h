#pragma once
// Who was hit with what, lately: per actor, per kind of damage, the last
// time and by whom, and the question the Hit by condition and the Attacker
// target ask of it. The game side feeds it from the engine's events and
// asks on the tick (game/Hits.cpp); the window and "the latest attacker"
// are decided here, where they are tested. No Skyrim.

#include "Kinds.h"
#include "Snapshot.h"

#include <array>
#include <cstdint>
#include <unordered_map>

namespace ft
{

// What has hit an actor within the window, as a bit per DamageKind, and
// who did it last.
struct Attacked
{
    std::uint8_t kinds{0};
    ActorId attacker{0};
};

class HitTable
{
  public:
    // A hit of `kind` on `target` by `attacker` (0 for nobody known) at
    // `when`, on whatever clock the caller keeps.
    void Note(ActorId target, DamageKind kind, ActorId attacker, double when);

    // The hits on `target` no older than `window` before `now`, and the
    // attacker of the latest of them.
    [[nodiscard]] Attacked Lately(ActorId target, double now, double window) const;

  private:
    struct Entry
    {
        double when{-1.0}; // -1: never hit with this kind
        ActorId attacker{0};
    };
    std::unordered_map<ActorId, std::array<Entry, static_cast<std::size_t>(DamageKind::COUNT)>> hits_;
};

} // namespace ft
