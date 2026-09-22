#pragma once
// A spell tome in a companion's pack, and the spell it teaches
// (dev/PROGRESSION.md, "Spells"). What a companion knows is
// progression/game/SpellView.h's. Game thread only.

#include "progression/core/Spells.h"

namespace fp::game
{

// The castable spell a book teaches, or none: a spell tome's, as the
// player's reading of one teaches it.
[[nodiscard]] RE::SpellItem *TomeSpell(RE::TESObjectBOOK *book);

// The spell as the ledger keeps it.
[[nodiscard]] SpellFacts FactsOf(RE::SpellItem *spell);

// How many of `book` the actor carries.
[[nodiscard]] int CarriedCount(RE::Actor *actor, RE::TESObjectBOOK *book);

} // namespace fp::game
