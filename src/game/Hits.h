#pragma once
// Who was hit with what, lately. The engine says so twice: a hit event for
// a blow or a spell landing, with the weapon or the spell as its source,
// and an effect-apply event for every magic effect that takes, cloaks and
// hazards included, which land without a hit. Both are kept in one table,
// per actor and per kind of damage, with the attacker and the time, and
// the snapshot asks it "attacked by fire in the last few seconds?" -- the
// Attacked by condition (docs/CONDITIONS.md 5).

#include "core/Kinds.h"
#include "core/Snapshot.h"

#include <cstdint>

namespace ft::game
{

// Once, at data load: listen for hits and effects.
void WatchHits();

// What has hit this actor within the window, as a bit per DamageKind, and
// who did it last. Any thread; the table is locked.
struct Attacked
{
    std::uint8_t kinds{0};
    ft::ActorId attacker{0};
};
[[nodiscard]] Attacked AttackedLately(ft::ActorId target);

// The actor value that resists a kind of damage -- ResistFire for Fire,
// PoisonResist for Poison, ResistMagic for Magic -- and kNone for the
// kinds nothing resists but armour (Melee, Ranged, Any). One table, read
// both ways: what a resistance is read off, and what an effect's resist
// value says of it.
[[nodiscard]] RE::ActorValue ResistValueOf(ft::DamageKind kind);

// The kind of damage a magic effect does, by what resists it: fire, frost,
// shock or poison, else Magic. What a hit is noted as; and what a spell, a
// staff or an enchantment in hand counts as using.
[[nodiscard]] ft::DamageKind KindOfEffect(const RE::EffectSetting *base);

} // namespace ft::game
