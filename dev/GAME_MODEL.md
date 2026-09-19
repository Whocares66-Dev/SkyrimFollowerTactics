# A model of the engine, for the tests

Part one -- the view and the decisions over it -- is built (2026-09-19, `src/core/BagView.h`); part two, the fake engine, is still a plan. What we would need to test the item-handling logic against the engine as we have measured it, without running Skyrim; what is already known well enough to write down; what has to be read or measured first. `dev/UNIQUE.md` holds the measurements themselves and stays the single record of them; this file says how they would become executable.

## Why

On 2026-09-12 the core tests were green all day and eleven bugs were found in play. Each was one of two kinds:

- **A fact about the engine we had not measured.** A shield carries the one Worn mark and not a weapon's left-hand mark; a copy handed over arrives with the player's ownership on a list of its own; the engine leaves a worn list where it is when asked to equip it into the other hand.
- **A decision made in the game layer from facts that were already readable.** Which list to hand the engine, whether a click is a move between hands, a holdable's count, whether a row stands apart, which hand a row's ticks are in. Each is a function of a few values read off the bag, and each sat in `src/game`, where nothing runs it but the game.

The core's rules were right and tested. Nothing tested that the game fed them what they were proved against, and nothing could exercise a decision that lived beside an `RE::` call. A model answers both: the decisions move into core over a plain view of the bag, and a fake engine supplies the bag under the measured rules, so today's eleven become scenario tests that run in seconds.

## What it is not

Not an emulator. The model may say only what has been read from the executable or measured in play, each rule citing where. Anything unmeasured is absent and a scenario that needs it fails loudly, which is the point: the gap is found at the desk, not in play. It is test-tree code; production keeps calling the real engine.

## Part one: the bag view, and the decisions over it

**The view.** A plain description of one form's copies in a bag, built in one place by the game from the real lists and by the tests from the fake bag:

| per row | what | read from |
|---|---|---|
| `variant` | the row's `ItemVariant` | `VariantOf` |
| `ownRow` | a row of its own on the Inventory tab, or one of the plain stack | `RowOfItsOwn` |
| `worn` | `None`, `Left`, `Right` (a shield's and armour's one mark reads as the kind decides) | `WornIn` |
| `stackable` | the engine's own verdict for its equip's first step | `IsInventoryStackable(true)` |
| `count` | copies on the list | the list's count |
| `token` | an opaque handle the game maps back to the list | the list's address |

plus the **listless remainder**: how many plain copies have no list at all, which only a null list can reach.

**The decisions**, each a pure function over the view, each with the bug of 2026-09-12 that would have been its first test:

| decision | today's bug |
|---|---|
| Is a list a row of its own, given its entry types and the stolen reading | three plain daggers, split on ownership |
| Which hand a row's marks put it in, by the object's kind | a shield's ban found nothing to take off; two rows ticked in both hands |
| Which copy an equip takes: the caller's kind of question (an exact row, the plain stack, or the variant), the hand, the incumbent, a copy in the other hand coming across, the stack before a row that shares the variant, a listless copy by null | the poisoned click landing on the clean one; the pinned poisoned dagger swapped out; the plain click readying the poisoned one; the tempered dagger not moving |
| Whether a click moves a variant's only copy between hands, and a holdable's count | the count taken from the form |
| Whether an engine equip is refused: bans, pins, the incumbent (a pin on a variant or on the form), the engine's pick minus the bans | the form-level pin's incumbent |

`RefusesEngineEquip` and `EnginePick` are in core already; the rest moves. The game keeps only what reads a list and what calls the engine.

## Part two: the fake engine

**State.** A bag: entries by form, each with a count and its lists in order; a list is an ordered set of typed entries with the values the model reads (Worn, WornLeft, Count, Hotkey, Ownership with its owner, Health, TextDisplayData with its text and whether the player set it, Enchantment as its effects, Poison, OutfitItem, FromAlias, UniqueID). Two hands and the armour slots. The player, for ownership.

**Behaviours**, each a method, each citing its source. The sources are in `dev/UNIQUE.md` unless noted; a behaviour without a source does not go in.

| behaviour | rule | source |
|---|---|---|
| equip with a list | the list, when it belongs to the entry, is used as is; a worn list stays where it is, whichever hand is asked; with a list not in the entry, the first list the comparison calls equal, else none | 16066, read live 2026-09-12; measured the same day |
| equip with no list | a copy the engine calls stackable first, else the first unworn list | 16066 |
| worn marks | a weapon gets Worn or WornLeft by hand; a shield, a torch and armour get Worn | measured 2026-09-12 |
| unequip | clears the hand's mark; an emptied list folds back into the stack | measured |
| hand-over | each copy arrives on a list of its own carrying the player's ownership; an empty arriving list is deleted and the copy joins the count; ids are cleared on the way out | measured; 16059 to 16065 |
| arrival merge | two arriving lists merge when they differ in nothing but count, hotkey and, under a flag, ownership | 11594. Contradicted once: two plain daggers stayed two lists of one. Open, below |
| the menu's rows | ownership ignored, worn folded, poison and tempering split | measured |
| tempering | health set in place on the copy's list; the id does not change | measured |
| poison | a dose on the worn copy's list; the row rejoins the stack when it wears off | measured in vanilla |
| stolen | the engine's ownership rule from the player's side | `IsOwnedBy`, not yet read; the model would carry the cases measured |
| enchanting at the table | a form minted per distinct recipe, found again by effects, magnitudes, durations and areas | `Effect::IsMatch` |

**Unknowns are absent.** Where a behaviour is unread the method throws or the scenario asserts it is not reached, and the case goes to the list below. The existing watchdog simulation in `tests/test_loadout.cpp` (`World`, `EngineEquip`, `Watchdog`) models pins and hands by form alone; it folds into the fake engine rather than standing beside it.

## The scenarios to write first

Each is one of today's, as a test over the fake bag, in the order they were found:

1. The plain dagger in one hand, the tempered in the other: each row ticks its own hand.
2. A worn shield banned comes off, and the cell returns to unequipped after the unban.
3. The poisoned row clicked readies the poisoned copy, not the clean one first in the entry's order.
4. Two plain daggers handed back are one row with the listless copy, count three.
5. The tempered dagger, the only one, pinned left and clicked right: the left lets go and it moves.
6. The tempered dagger clicked right while a plain one holds the left: the plain one stays, its pin stays.
7. The poisoned dagger in the hand, pinned there: it stays, the pin shows on it.
8. A rule's pin on the form with a copy worn: the engine's no-list equip into that hand is refused.
9. A pinned dagger the engine moved to the other hand comes back across.
10. The plain stack clicked while the poisoned one holds the hand: a clean one goes in.
11. A row clicked while worn in the other hand moves, whatever the variant's count.

## To read or measure before building

`dev/DEVBENCH.md`, "How it relates to the game model", is how these could be measured by script against the running game instead of by hand.

- **The engine's no-list pick with the stack listless and a stackable row beside it.** Null is the only way to name a listless copy; whether the engine's first step takes it or the poisoned list first is unread. The debug line after such an equip says.
- **What the menu splits on, in full.** `InventoryEntryData::NormalizeAndCountNonStackableExtraLists` and `ExtraDataList::IsInventoryStackable` are the candidates. The scan's debug line gives the engine's stackable verdict per list shape; read it against the menu for the outfit mark (`8E`), the alias mark (`95`), a leveled-list mark, and a poisoned list.
- **The arrival merge.** Circlets handed over separately merged into one list; two plain daggers did not. Whether the difference is the flag, the ownership, or the order of arrival decides how the model's hand-over behaves.
- **Stolen in the menu.** Whether a stolen copy is its own row in the game's screen, as the variant assumes.
- **The ownership rule.** What `IsOwnedBy` answers for the player's ownership, a follower's own, a faction's, and none, so the model's stolen cases are the engine's.
- **A follower's own loot.** What a copy the follower picks up carries, if anything.

## Where it would live

- `src/core/BagView.h`: the view and the decisions over it, tested in `tests/test_bag.cpp` by hand-built views first, before the fake engine exists. **Built 2026-09-19** (`wip-game-test`): the view, the questions over it and `PlanEquip`, the game switched over, nothing behaving differently; not yet run in play since the switch.
- `tests/model/`: the fake engine, one file for the state and one for the behaviours, each behaviour's comment its citation.
- `dev/UNIQUE.md` stays the record of measurements and gains a line per new one; this file points there and never repeats a number.

## Order

1. The view and the decisions in core, the game switched over to them, nothing behaving differently. Tests over hand-built views for each decision. **Done 2026-09-19.**
2. The fake engine with the behaviours that have a source today, the watchdog simulation folded in.
3. The eleven scenarios, then every open item above as it is read.

Each step is its own commit and the game builds green at every one.
