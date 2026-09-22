#pragma once
// Spell tomes in the player's pack, the spells a companion knows, and what
// teaching and forgetting do to the actor (dev/PROGRESSION.md, "Spells").
// What a companion knows is progression/game/SpellView.h's; this is the rest. Game
// thread only.

#include "progression/core/Ids.h"
#include "progression/core/Spells.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fp::game
{

struct Tome
{
    FormKey book;
    std::string bookName;
    SpellFacts facts;
    int count{0};
};

// Every book in the player's inventory that teaches an ordinary school
// spell, one row per spell (two books teaching one spell are one row).
// `forActor` prices the spell as that companion would cast it.
[[nodiscard]] std::vector<Tome> TomesCarried(RE::Actor *forActor);

struct KnownSpell
{
    SpellFacts facts;
    bool onRecord{false}; // on their own base record, rather than added to them
};

// Every ordinary spell the actor has in the engine's own lists -- their
// record's, and those added to them (by a quest, by another mod) -- read
// from the lists rather than asked through VisitSpells, so one of theirs set
// aside is still listed; then `taught`, which only the view holds.
[[nodiscard]] std::vector<KnownSpell> KnownSpells(RE::Actor *actor, std::span<RE::SpellItem *const> taught);

// Is the spell on the actor's own record?
[[nodiscard]] bool SpellOnRecord(RE::Actor *actor, const RE::SpellItem *spell);

[[nodiscard]] SpellFacts FactsOf(RE::SpellItem *spell, RE::Actor *caster);

// One tome taken from the player.
bool TakeTome(RE::TESObjectBOOK *tome);

// Without the spell hooks (VR), a taught spell goes onto the actor as the
// engine keeps it, and comes off the same way. With them, forgetting only
// takes it out of their hands: knowing it was the view's.
bool AddToActor(RE::Actor *actor, RE::SpellItem *spell);
bool ForgetSpell(RE::Actor *actor, RE::SpellItem *spell);

} // namespace fp::game
