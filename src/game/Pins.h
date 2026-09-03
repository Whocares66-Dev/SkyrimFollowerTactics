#pragma once
// Pins: what stays in a hand, and how the promise is kept.
//
// A pin is the one thing a person authors here. The rules that follow from
// it -- which hands a thing takes, what a pin releases, what the combat AI
// must not be offered, what cannot be pinned -- are core/Loadout.h, pure and
// tested. This is the game side: the pins themselves, the requests the
// panel makes, and the tick's work of keeping the promise. Game thread only.

#include "core/Loadout.h"
#include "game/Inventory.h"
#include "game/Magic.h"

#include <cstdint>
#include <vector>

namespace RE
{
class Actor;
}

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

// The tick's part. Mark the scanned items and spells that are pinned, and
// those the AI is kept from, for the panel; drop pins for things gone.
void MarkPins(RE::Actor *actor, std::vector<InventoryItem> &items, std::vector<MagicEntry> &magic);

// Keep the promise: put back what the game took off (hands stand down in
// combat), prune the combat AI's list to the pins and ready them, and log
// what the AI is choosing from once per fight.
void KeepPins(const std::vector<RE::Actor *> &followers);

// Republish the views owed after a spell left a hand: the Papyrus native
// that does it runs a frame after the request. Called first thing in the
// tick, ahead of every hold.
void RepublishOwed();

} // namespace ft::game
