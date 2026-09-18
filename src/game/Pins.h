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
// takes it off, until the item leaves their inventory or a later request lets
// go. The game re-dresses a follower freely -- their default outfit comes
// back on a cell change, and a better piece of armour handed over is worn
// at once -- and a pin is how "wear this" survives that without touching
// their outfit record, which is what the heavier follower frameworks do.
// Pinning releases any pin it conflicts with (same body slot, other hand),
// since the engine will not displace a pinned item on its own.
//
// A pinned weapon, spell or torch is held in and out of combat alike, with
// one exception: while a CastSpell rule's package has a hand for its spell,
// the pin in that hand waits, and goes back once the spell has left it. A
// cast borrows the hand; the pin is what the follower fights with
// otherwise. Armour and ammunition hold throughout.
//
// Equip puts it on with no promise: the AI's to change.
//
// Unequip takes it off and touches neither pins nor bans: the player's
// page, whose cells only equip and unequip, since nothing chooses for the
// player. A follower's pinned thing taken off so would come back.
//
// Ban takes it off and keeps it off: the combat AI scores it zero, the
// engine's own equips of it are refused, and the watchdog takes it off if
// it is found on. A ban lets go of any pin on the thing. Unban forgets
// the ban; the thing stays off until something puts it on. Bans are not
// a fight's business: the book of bans is one, before, during and after.
// A rule's pin on a banned thing overrides the ban for the fight, and no
// longer: the pin goes with the fight's others, and the watchdog's ban
// pass, later in the same tick, finds the thing on and unpinned and takes
// it off. What was pinned in its place before the fight comes back by the
// restore; anything else stays off, and a Combat end rule is how to want
// it back at once.
//
// A fight does not rewrite the book. What is pinned when a follower enters
// combat is remembered and put back when combat ends: the rules' pins for
// the fight are let go in place -- the gear stays on, the AI's to change
// -- or taken off where a pin from before the fight is coming back, and
// the player's pins are pinned again and put back on. A pin the player
// makes in the panel mid-fight counts as the new normal and survives.
//
// A pin or a ban names one VARIANT of an item where the bag holds several
// rows of a form (Holdable::variant; dev/UNIQUE.md "The variant"), so the
// enchanted armour is pinned and the outfit one is not; a rule's pin may
// name the form alone, whichever variant. The three means see it unevenly,
// and none of them chooses a copy: ours is to gatekeep the pins and the
// bans, and the engine's tie-breaks are left to it. The equip detour
// judges an equip that names a list by that list's variant -- a banned
// one, or another variant into a pinned hand or slot, is refused; a copy
// of a pinned variant already worn there is the incumbent, and a no-list
// equip or another copy of the same variant aimed at it is refused too --
// and gives one that names no list (the combat AI's) the row the engine
// would itself have reached with the banned variants left out of its
// pool: the engine's order, less the bans (core's EnginePick). The combat
// AI's list is by form, so the score hook shadows by form for a pin, and
// zeroes a form for a ban only when every row carried is banned. The
// watchdog finds a worn row of the pinned variant and, with none, dresses
// one by the same pick; takes off a banned row found on; and drops a pin
// or ban whose variant has no row left in the bag.
//
// Independent of tactics: pins are enforced whether the tactics switch is on
// or off, on the same half-second clock, by a watchdog that looks only at the
// pinned items and does nothing at all when nothing is pinned.
//
// Only while they are in the player's service. A follower dismissed keeps
// their pins and bans, in the book and in the save, and none of the three
// means applies them: the watchdog walks the tick's followers, who are
// teammates, and the equip detour and the score hook pass anyone who is not
// one. Rejoining, the book applies again, and the watchdog's first pass puts
// the pinned things back on and takes the banned ones off.
//
// Callable from any thread: the work is queued to the game thread. The
// pins go into the save with the rules (game/Profiles.h).
enum class WearRequest
{
    Equip,
    Unequip,
    Pin,
    Ban,
    Unban
};

// `variant` is which row of the form; none for the form itself. `row` is
// the clicked row's own list (InventoryItem::row) when it has one: the
// pin or ban is on the variant, and the equip goes to that row, so a click
// on the poisoned dagger readies the poisoned dagger and not the clean
// one beside it, which is the same variant.
void RequestWear(ft::ActorId id, std::uint32_t form, WearRequest request, Hand hand = Hand::None,
                 std::optional<ft::ItemVariant> variant = std::nullopt, RE::ExtraDataList *row = nullptr);

// The rules' side of the same book, on the game thread, from the tick. A
// rule's pin is the panel's pin: it goes in the same book, shows in the
// same cells, and the panel can let it go. Pin puts `form` in `hand` --
// Both for an either-hand spell is once in each hand -- and returns whether
// the form was found and the pin taken: a spell above the follower's skill
// is refused. Release lets go of every pin of `kind` and takes
// those things off, so the AI decides again.
bool PinNow(RE::Actor *actor, std::uint32_t form, Hand hand,
            const std::optional<ft::ItemVariant> &variant = std::nullopt);
// Let go of every pin of the kind, and take off whatever of the kind is on,
// pinned or not -- a weapon or spell in the hands named, the arrows in the
// quiver, every piece of armour worn -- so the AI decides again from empty;
// `hands` narrows a weapon's or a spell's release to one hand (None: every
// hand).
void ReleaseKind(RE::Actor *actor, Kind kind, Hand hands = Hand::None);

// One request against the book now, on the game thread, for the rules: the
// panel's RequestWear without the queue. A plain Equip is what the player's
// rules make -- a pin is a leash on a combat AI the player does not run.
bool WearNow(RE::Actor *actor, std::uint32_t form, WearRequest request, Hand hand,
             const std::optional<ft::ItemVariant> &variant = std::nullopt);

// What the actor has on and in hand, worded as pins: each hand's thing
// with its copy, the armour worn, the ammunition. The player's snapshot
// carries these where a follower's carries the book, so an equip rule of
// theirs is done when the thing is worn, as a follower's is when it is
// pinned, and nothing else in the evaluator changes. Game thread.
[[nodiscard]] std::vector<Pin> WornAsPins(RE::Actor *actor);

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

// The engine's own unequip of a spell from a hand (source 0 the left, 1 the
// right, 2 the voice) and of a shout, on this frame: what the Papyrus
// natives tail-call, reached by ID (game/Addresses.h). What gives the
// player's hand back after a cast lent it (game/PlayerCast.h).
void UnequipSpellNow(RE::Actor *actor, RE::SpellItem *spell, std::uint32_t source);
void UnequipShoutNow(RE::Actor *actor, RE::TESShout *shout);

// The planner's description of a form: what it is, which hands its record
// lets it take, whether the combat AI would choose it, which body slots it
// covers; and of one variant of it, or the form with none. The count is
// the variant's. The ONLY place the pin rules meet a record.
[[nodiscard]] Holdable DescribeHoldable(RE::Actor *actor, RE::TESForm *form,
                                        const std::optional<ft::ItemVariant> &variant = std::nullopt);

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

// Once, at data load: detour the engine's three equips -- an item, a spell
// into a hand, a shout or power into the voice -- so that an equip of ITS
// choosing, or a script's or a package's, is refused when it names a
// banned thing or would take a hand or slot a pin holds. Our own equips
// pass, and so does the spell or shout a record of ours is casting. This
// is what holds a pin and a ban out of combat and in it: the engine's
// prevent-removal flag is deliberately not used (EquipPinned says why),
// and the unequip is not detoured, since a refused removal cannot tell a
// swap from an item leaving the bag, and corrupted the hands when the flag
// tried (2026-09-04). The approach Follower Equip Control ships.
void RefuseEquipsAgainstPins();

} // namespace ft::game
