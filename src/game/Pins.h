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
// A pinned weapon or torch is held only OUT of combat. In a fight the hands
// are the combat AI's and the rules' -- a spell wants one -- and holding a
// dagger there against them only flickers. It goes back on when the fight
// ends. Armour and ammunition hold throughout.
//
// Unpin leaves it worn but hers to change again. TakeOff takes it off and
// forgets it; the game may put it back, and what it wears by default is
// its business.
//
// Independent of tactics: pins are enforced whether the tactics switch is on
// or off, on the same half-second clock, by a watchdog that looks only at the
// pinned items and does nothing at all when nothing is pinned.
//
// Callable from any thread: the work is queued to the game thread. Pins live
// in memory only, like the rules, until profiles persist.
enum class WearRequest
{
    Pin,
    Equip, // on, but no pin: for a spell the AI would not choose, where a pin would be a promise unkept
    Unpin,
    TakeOff
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

// The tick's part. Mark the scanned items and spells that are pinned, and
// those the AI is kept from, for the panel; drop pins for things gone.
void MarkPins(RE::Actor *actor, std::vector<InventoryItem> &items, std::vector<MagicEntry> &magic);

// Keep the promise on the tick: put back what the game took off (hands
// stand down in combat), and log what the AI is choosing from once per
// fight. In a fight the promise is kept by WatchCombatScores, not here.
void KeepPins(const std::vector<RE::Actor *> &followers);

// Once, at data load: take over the scoring of every kind of entry in the
// combat AI's list of options, so an entry the pins keep from the AI
// scores zero whenever the AI asks, and is never chosen. Reactive: nothing
// is computed until the AI asks, and nothing on the tick.
void WatchCombatScores();

// Once, at data load: detour the engine's equip so that an equip of ITS
// choosing -- the best weapon on leaving combat, the outfit on a cell
// change, a better arrow -- is refused when it would take a hand or slot a
// pin holds. Our own equips pass. This is what holds a pinned SPELL out of
// combat, where it has no prevent-removal flag to hold it; for items it
// doubles the flag. The approach Follower Equip Control ships.
void RefuseEquipsAgainstPins();

// Republish the views owed after a spell left a hand: the Papyrus native
// that does it runs a frame after the request. Called first thing in the
// tick, ahead of every hold.
void RepublishOwed();

} // namespace ft::game
