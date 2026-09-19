#pragma once
// A follower's magic, worded for the panel: the spells they know, their powers,
// their shouts. Built on the game thread, copied with the view.

#include "core/Loadout.h"
#include "core/Views.h"
#include "game/Sensors.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
class Actor;
class SpellItem;
class TESShout;
class TESWordOfPower;
} // namespace RE

namespace ft::game
{

// The categories a follower's magic is sorted into: the five schools, then
// what has no school, then the two that ride the voice slot. A follower has
// no favourites, and what is running on them has a tab of its own.

[[nodiscard]] const char *DisplayName(MagicCategory category);

// Has the player unlocked this word of power? Words are unlocked once, for
// everyone, so the answer is the same for every follower.
[[nodiscard]] bool WordUnlocked(const RE::TESWordOfPower *word);
// How far a shout's unlocked words reach: the highest variation whose word
// the player has unlocked, -1 for a shout with none, which nobody can
// shout. Vanilla unlocks the words in order, so this is the count of them
// less one; a shout whose second word alone is unlocked answers 1, which is
// as far as the words go either way.
[[nodiscard]] int HighestUnlockedWord(const RE::TESShout *shout);

// The words a spell's page and lists use, shared with a scroll's, which is
// a spell in a wrapper: the school of a skill; the kind of spell from its
// costliest effect (the element where it does that kind of damage, else
// the archetype); how it is cast, delivery and casting type in one word.
[[nodiscard]] MagicCategory SchoolOf(RE::ActorValue skill);
[[nodiscard]] std::string TypeWord(const RE::EffectSetting *base);
[[nodiscard]] const char *CastWord(RE::MagicSystem::Delivery delivery, RE::MagicSystem::CastingType casting);

// Everything castable they have, sorted by name. Abilities, diseases and the
// like are left out, as the magic menu leaves them out.
[[nodiscard]] std::vector<MagicEntry> ScanMagic(RE::Actor *actor);

} // namespace ft::game
