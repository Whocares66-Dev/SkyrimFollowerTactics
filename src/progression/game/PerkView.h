#pragma once
// A companion's perks as the engine sees them: the base record's perks,
// less the ones set aside, plus the ones bought here -- without editing the
// base record, which every copy of an NPC shares (dev/ENGINE_PERKS.md).
//
// Two of Character's virtuals are replaced, and pass straight through for
// every actor that is not a managed companion:
//
//   ForEachPerk          what "which perks does this actor hold" walks --
//                        the engine's HasPerk is this walk with a finder, so
//                        conditions, Papyrus and the perk menus all agree
//   ApplyPerksFromBase   what registers a perk's effects on the actor's
//                        process when that process is built
//
// A change while the actor is loaded goes through the engine's own queued
// rank change, the call ApplyPerksFromBase itself makes. The record is never
// written and the view is not saved: it is rebuilt from the ledger. What an
// ability perk registers may be (dev/ENGINE_PERKS.md), which is what
// turning levelling off takes back.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace fp::game::perkview
{

struct Diff
{
    std::vector<RE::BGSPerk *> removed; // on the record, set aside
    std::vector<RE::BGSPerk *> added;   // bought here
};

// At data load: patch the two slots. Safe with nobody managed. Not on VR.
void Install();
[[nodiscard]] bool Installed() noexcept;

// Game thread: the view for every managed actor, by the reference's runtime
// id. Replaces the last one whole; an actor not in it, or with an empty
// diff, is answered from its record.
void Publish(std::unordered_map<RE::FormID, Diff> diffs);

// Game thread: bring a loaded actor's registered effects in line with its
// view -- or with its record, if it had a view and no longer does -- through
// queued rank changes. Nothing for an actor with no high process (nothing is
// registered then).
void Reconcile(RE::Actor *actor);

// Game thread, before a load or a new game: no views, nothing remembered.
void Forget();

// What the engine will answer for the actor: the record's perks less the
// set-aside ones, plus the added ones.
[[nodiscard]] std::vector<RE::BGSPerk *> Effective(RE::Actor *actor);

} // namespace fp::game::perkview
