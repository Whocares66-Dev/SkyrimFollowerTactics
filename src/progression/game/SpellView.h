#pragma once
// A companion's spells as the engine sees them: everything they know, less
// the ones of their own set aside, plus the ones taught here -- without
// editing their record, which every copy of the NPC shares, and without
// adding to the actor, which the save would keep (dev/ENGINE_SPELLS.md).
//
// Two engine functions are replaced, and pass straight through for every
// actor that is not a managed companion:
//
//   VisitSpells   what "which spells does this actor know" walks: HasSpell
//                 (conditions, Papyrus), AddSpell's already-known check,
//                 and the combat AI's gathering of what it may cast.
//                 Detoured, since it is not virtual.
//   CheckCast     Character's "can it cast this now": refused for a spell
//                 set aside, which keeps the UseMagic package procedure --
//                 the one reader of the record's spells that does not walk
//                 through VisitSpells -- from choosing it.
//
// Only castable spells are set aside or added. Abilities are cast from the
// record by other walks (dev/ENGINE_SPELLS.md) and are left alone.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fp::game::spellview
{

struct Diff
{
    std::vector<RE::SpellItem *> removed; // theirs, set aside
    std::vector<RE::SpellItem *> added;   // taught here
};

// At data load. Safe with nobody managed. Not on VR.
void Install();
[[nodiscard]] bool Installed() noexcept;

// Game thread: every managed actor's view, by the reference's runtime id,
// replacing the last whole. An actor not in it knows what the engine says.
void Publish(std::unordered_map<RE::FormID, Diff> diffs);

// Game thread: a taught spell forgotten or released. The view no longer
// hands it over, but a fight's combat inventory, gathered before, still
// lists it: until the actor is out of combat, CheckCast refuses it and
// Reconcile keeps it out of their hands.
void Withdraw(RE::Actor *actor, RE::SpellItem *spell);

// Game thread, each tick for a loaded companion: a spell set aside or
// withdrawn is taken out of the actor's hands and voice, and out of a
// UseMagic package's choice, where a save or a fight may have left it.
void Reconcile(RE::Actor *actor);

// Game thread, before a load or a new game: no views.
void Forget();

} // namespace fp::game::spellview
