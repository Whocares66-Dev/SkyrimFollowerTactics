#pragma once
// Pins: what stays in a hand, and how the promise is kept.
//
// A pin is the one thing a person authors here. The rules that follow from
// it -- which hands a thing takes, what a pin releases, what the combat AI
// must not be offered, what cannot be pinned -- are core/Loadout.h, pure and
// tested. This is the game side: the pins themselves, the requests the
// panel makes, the watchdog on the tick, and the hook that answers the
// combat AI when it asks what to hold. Game thread only.

#include "core/Loadout.h"
#include "core/Profile.h"
#include "game/Inventory.h"
#include "game/Magic.h"

#include <cstdint>
#include <vector>

namespace RE
{
class Actor;
class SpellItem;
} // namespace RE

namespace ft::game
{

// Put an item or a spell on, keep it on, or take it off, from the panel.
//
// Pin equips it and KEEPS it on: the tick puts it back whenever the game
// takes it off, until the item leaves her inventory or a later request lets
// go. The game re-dresses a follower freely -- her default outfit comes
// back on a cell change, and a better piece of armour handed over is worn
// at once -- and a pin is how "wear this" survives that without touching
// her outfit record, which is what the heavier follower frameworks do.
// Pinning releases any pin it conflicts with (same body slot, other hand),
// since the engine will not displace a pinned item on its own.
//
// A pinned weapon, spell or torch is held in and out of combat alike, with
// one exception: while a CastSpell rule's package has a hand for its spell,
// the pin in that hand waits, and goes back once the spell has left it. A
// cast borrows the hand; the pin is what the follower fights with
// otherwise. Armour and ammunition hold throughout.
//
// Equip puts it on with no promise: the AI's to change. Unpin leaves it
// worn but hers to change again. TakeOff takes it off and forgets it; the
// game may put it back, and what it wears by default is its business.
//
// Ban takes it off and keeps it off: the combat AI scores it zero, the
// engine's own equips of it are refused, and the watchdog takes it off if
// it is found on. A ban lets go of any pin on the thing. Unban forgets
// the ban; the thing stays off until something puts it on. Bans are not
// a fight's business: the book of bans is one, before, during and after.
//
// A fight does not rewrite the book. What is pinned when a follower enters
// combat is remembered and put back when combat ends: the rules' pins for
// the fight are let go in place -- the gear stays on, the AI's to change
// -- or taken off where a pin from before the fight is coming back, and
// the player's pins are pinned again and put back on. A pin the player
// makes in the panel mid-fight counts as the new normal and survives.
//
// Independent of tactics: pins are enforced whether the tactics switch is on
// or off, on the same half-second clock, by a watchdog that looks only at the
// pinned items and does nothing at all when nothing is pinned.
//
// Callable from any thread: the work is queued to the game thread. The
// pins go into the save with the rules (game/Profiles.h).
enum class WearRequest
{
    Equip,
    Pin,
    Unpin,
    TakeOff,
    Ban,
    Unban
};

void RequestWear(ft::ActorId id, std::uint32_t form, WearRequest request, Hand hand = Hand::None);

// The rules' side of the same book, on the game thread, from the tick. A
// rule's pin is the panel's pin: it goes in the same book, shows in the
// same cells, and the panel can let it go. Pin puts `form` in `hand` --
// Both for an either-hand spell is once in each hand -- and returns whether
// the form was found. Release lets go of every pin of `kind` and takes
// those things off, so the AI decides again.
bool PinNow(RE::Actor *actor, std::uint32_t form, Hand hand);
void ReleaseKind(RE::Actor *actor, Kind kind);

// This follower's pins, as the planner and the snapshot take them.
[[nodiscard]] std::vector<Pin> PinsOf(ft::ActorId id);

// The player's pins, for the save: the book as the panel left it, which
// in a fight is the one remembered for after it, not the one the rules
// are using. Game thread.
[[nodiscard]] std::vector<ft::PinEntry> PlayerPinsOf(ft::ActorId id);

// This follower's bans, as the hooks and the save take them.
[[nodiscard]] Bans BansOf(ft::ActorId id);

// The bans from the follower's saved record, taken back whole: a ban
// promises what is NOT worn, and that holds for anything that still
// exists. Game thread, at first sight; the watchdog's first pass takes
// off whatever a banned thing is found on.
void AdoptBans(RE::Actor *actor, const Bans &bans);

// The pins from the follower's saved record, taken back into the book --
// each only if the follower still has the thing on, in those hands; a pin
// is a promise about what is worn, and a load re-dresses nobody. One that
// does not hold is logged and forgotten: the thing is gone, or the save
// was played on without the mod. Game thread, at first sight, before the
// watchdog's first pass.
void AdoptPins(RE::Actor *actor, const std::vector<ft::PinEntry> &pins);

// Forget every book, pins and bans: before a save loads, and on a new game.
void ForgetPins();

// The planner's description of a form: what it is, which hands its record
// lets it take, whether the combat AI would choose it, which body slots it
// covers. The ONLY place the pin rules meet a record.
[[nodiscard]] Holdable DescribeHoldable(RE::Actor *actor, RE::TESForm *form);

// Put a spell in a hand -- Left, Right, or None for the engine's choice --
// unless it is there already. The engine's item equip is a no-op for an
// item already worn; its spell equip is not, and each call plays the equip
// sound, so the panel and the watchdog together could sound several times
// for one pin. Returns whether anything was done.
bool EquipSpellIn(RE::Actor *actor, RE::SpellItem *spell, Hand hand);

// The tick's part. Mark the scanned items and spells that are pinned or
// banned, and those the AI is kept from, for the panel; drop pins for
// things gone.
void MarkPins(RE::Actor *actor, std::vector<InventoryItem> &items, std::vector<MagicEntry> &magic);

// Keep the promise on the tick: put back what the game took off, in a
// fight or out of one (a hand pin waits only while our own cast holds the
// hand -- PutBackNow), and log what the AI is choosing from once per fight.
// Corrective, where WatchCombatScores and RefuseEquipsAgainstPins are
// preventive: a spell equip gets past both, and this is what answers it.
void KeepPins(const std::vector<RE::Actor *> &followers);

// Once, at data load: take over the scoring of every kind of entry in the
// combat AI's list of options, so an entry the pins keep from the AI
// scores zero whenever the AI asks, and is never chosen. Reactive: nothing
// is computed until the AI asks, and nothing on the tick.
void WatchCombatScores();

// Once, at data load: detour the engine's equip so that an equip of ITS
// choosing -- the best weapon on leaving combat, the outfit on a cell
// change, a better arrow -- is refused when it would take a hand or slot a
// pin holds. Our own equips pass. This is what holds a pin out of combat,
// item or spell: the engine's prevent-removal flag is deliberately not
// used (EquipPinned says why). The approach Follower Equip Control ships.
void RefuseEquipsAgainstPins();

// Republish the views owed after a spell left a hand: the Papyrus native
// that does it runs a frame after the request. Called first thing in the
// tick, ahead of every hold.
void RepublishOwed();

} // namespace ft::game
