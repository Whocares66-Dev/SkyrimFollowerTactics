# Which copy: how one item in a follower's bag is told from another

A follower's bag holds one entry per base form, and the panel, the pins, the bans and the rules all need to say *which* iron dagger when there are two kinds. This is what the engine gives us to say it with, what we do with it, and what is still open. Read alongside `docs/PROFILES.md` (the wire format) and `src/core/Loadout.h` (the rules, tested).

## What the engine has

**One entry per form.** A container's inventory is a list of entries, one per base form (the record: `Iron Dagger`, `0x12EB7`), each with a count. Nothing more is kept for a copy that is like every other: three plain iron daggers picked up in a dungeon are the entry with a count of 3.

**An extra-data list per copy that differs.** A copy the engine has to record something about gets a list of its own hung off the entry: its enchantment (a player's, at an enchanter), its tempering, a custom name, a poison dose, a charge, a soul, whether it is worn (`ExtraWorn` / `ExtraWornLeft`), whether the outfit put it there (`ExtraOutfitItem`). Copies alike in everything the engine records share one list with a count on it, whenever they arrived: a second normal dagger handed over another day joins the first's list (measured 2026-09-12; the id comparison in the header had predicted otherwise). The engine's own menu groups by these lists: a row per list, and one row for the listless remainder.

```
Entry: Iron Dagger, count 4
   |-- (no list)  x1     an outfit or leveled-list copy: a number on the entry
   |-- List A  id 10 x2  the normal daggers the player handed over, both
   |-- List B  id 11 x1  the smithed one: tempering on the list
```

**A unique id, on a list.** `ExtraUniqueID` is one more entry on a list: the form of the container that issued it, and a 16-bit number. Measured, 2026-09-11 and 2026-09-12:

- **Issued on arrival -- in Nordic Souls. Not in a mostly vanilla load order.** In Nordic Souls anything handed to a follower got an id, plain or not, and a listless copy was given a list to hold it: a plain iron dagger given to Frea had a list with an id and nothing else on it. In the mostly vanilla test instance (2026-09-12, Jenassa), a tempered Iron Dagger (Fine) handed over has **no id at all**. The engine's add path, as read, reissues an id for an arriving list only when the list already carries one, or the form has a particular flag; it mints none for an ordinary item. And on the Nordic Souls process the function that sends the container-changed event (16149) began with a jump through a pointer outside the game image: another plugin's hook, clean in the vanilla instance. A plugin sitting on that event sees every arrival and can issue an id for it there. So the ids we measured there were that plugin's doing, not the engine's, and a design that waits for the engine to issue ids has nothing to key on in vanilla. Which plugin, and whether it is one this mod could reasonably require, is not yet known.
- **The lowest free number in the bag, every time.** Read from the running executable, 2026-09-12: both id generators (16147, 16148) collect the ids in use in the bag into a set and count up from 1 to the first not in it. There is no counter that only increases: with 1, 2 and 3 present and 2 gone, the next arrival is 2. Two iron daggers given in one order got 10 and 11; taken back and given in the other order, the other one got 10. The `TESUniqueIDChangeEvent` exists because of this. An id says nothing about age or history.
- **Identical copies share one list and one id -- sometimes.** Measured: two Copper and Moonstone Circlets handed over separately read as one row, count 2, id 10. But two plain iron daggers handed back on the vanilla instance (2026-09-12) stayed two lists of one, each carrying the player's ownership and nothing else, which the game's own menu shows as one stack of three with the listless copy. The add path reissues an arriving list's id early and its merge comparison (11594) skips only count, hotkey and, under a flag, ownership -- not the id -- so the merge of two listed copies with different ids is not where this happens; the likelier path is a listless arrival joining the entry and the id being settled on the merged list afterwards. Not read to the end; the observable rule is what the design rests on.
- **Kept while the copy stays, cleared by the engine when it leaves.** Tempering did not change an id. The removal path (16059, through 16065) reads the list's id and, if it has one, deletes the entry and raises the unique-id change event. Read on both the Nordic Souls and the vanilla instance, identical and unhooked: this half is the engine's, whoever issued the id.
- **Not issued to what the engine makes in place.** An outfit piece the follower spawned with has a list (the outfit mark, the worn mark) and no id. A leveled-list item is expected to be the same; not yet read.
- **Not what the engine matches copies by.** The list's contents are; the id is a label the engine keeps for the quest system and for events, and it happens to be exactly what a pin needs.

**What the equip call takes.** Every equip in the game, the AI's, a script's, a package's, ours, lands in `ActorEquipManager::EquipObject(actor, object, extraList, ...)`. The list argument names the copy; null names the form and leaves the copy to the engine. The combat AI's own list of options (`CombatInventoryItem`) holds a form, a score and a slot, and no list: it cannot tell copies apart, and its scoring can only use the form's data.

**The engine's own equips are the same call.** Read 2026-09-12: the routine that re-arms a follower with the best weapon or dresses them on a gift (39650, from 39637) is decision logic over four calls to `EquipObject`, each with the form, a null list, a count of one, the slot, the queue flag set and sounds on, and nothing of its own before or after. The vanilla follower screen equips nothing on the follower; giving hands the decision to that routine. Everything item-side -- the worn mark, the enchantment's abilities, the sound -- happens inside `EquipObject`, for the engine's calls and ours alike. Ours differ in naming the copy's list, and, from a panel click, in the immediate flag and a model refresh, because the frozen clock withholds the queued update.

**Which copy the engine takes for a null list.** Read from the running executable, 2026-09-12 (`tools/livedisasm.py`; the path is `EquipObject` 38894, its worker 38929, the immediate equip 38001, the actor's core equip 38004, and the inventory's own equip 16066, where the choice is made). The inventory routine counts the copies that are on no list at all, then:

1. **A plain copy first.** "Plain" is decided by a counting function (11598) that walks a list's extras against a table of types: a list holding only worn marks, or only extras the table calls indifferent -- tempering (`ExtraHealth`), charge, a poison dose, a custom name among them -- is no list to it, and its copies count as plain alongside the truly listless ones. A list holding a **unique id**, an **outfit mark**, an enchantment, a soul, a count, ownership, a hotkey, a leveled-list mark or a scale is a distinct copy. If plain copies remain, the engine makes a new list for one, puts the worn mark on it, and hangs it on the entry; that list carries no id, so a worn plain copy stays plain.
2. **Else the first list in the entry's order that is not already worn.** One copy is split off a stack with more than one.

So an outfit piece is *not* plain to the engine: its outfit mark makes it a distinct copy, and with the outfit armour worn and a tempered gift beside it, the gift is the first unworn list and is what the engine takes when the AI asks for the form. Whatever keeps followers in their outfit piece over a better copy of it is therefore not this rule; the likelier causes are the AI's equip-best routine not asking about a form already worn, and the outfit system's own re-dress.

A list that is given is used as given, after a check that it belongs to the entry (or a list equal to it, by the extra data's own comparison, is used instead). The combat AI always gives null, so for the AI this rule is the whole answer. Note what "plain" means here: **listless**. A normal iron dagger the player handed over has a list carrying its id, like the smithed one beside it, so neither is plain to this rule, and the AI gets whichever list hangs first on the entry, most likely the one that arrived first (which end the engine adds to is not read). The AI has no preference between copies at all; it does not know there are two. Listless copies are what the follower got without a hand-over: an outfit piece, a leveled-list item.

## The variant

A pin, a ban or an equip action names a **row as the player sees it**: the form, and the row's **variant**, which is written down as the entries on the copy's list that only the player's own hand changes:

| part | from | why it is in the variant |
|---|---|---|
| the form | the record | the kind of thing |
| enchantment | `ExtraEnchantment`, as its **effects**: each effect's record with the magnitude, duration and area the enchanter set | put on at an enchanter, fixed after. An enchantment made at the table is a form the game mints in the save, one per distinct recipe, and finds again by these same fields (`Effect::IsMatch`); its number belongs to the save, not to any plugin, so the recipe is what tells enchantments apart, not the number. Two enchantments alike in effects and strengths are one; Resist Fire 25% and 50% are two; an effect added or left out is another |
| tempering | `ExtraHealth`, the multiplier | changes only at a grindstone, which ends the pin: a (+5) is not the (+4) asked for |
| custom name | `ExtraTextDisplayData` | given at an enchanter or by renaming |
| stolen | `ExtraOwnership`, asked through the engine's own ownership rule (`IsOwnedBy`) from the player's side: yes or no, never who | a stolen copy is its own row in the game's screen, and stays one until fenced. Ownership as such is not a part: every copy handed to a follower carries the player's ownership (`0x21`, measured 2026-09-12), and the game's menu stacks it with the follower's own copies |

Left out on purpose: **ownership as such**, which a hand-over writes on every copy and the game's menu ignores; **charge**, which drains as the weapon is used; a **poison dose**, which the follower's own rules put on and which wears off; **worn marks, count and the favourites hotkey**, which the engine itself ignores when it decides what stacks; and the **unique id**, which the engine hands out per bag and takes back on leaving (below), and which vanilla never issues to ordinary gear at all. Spells, powers and shouts have no lists and no copies: the form is all there is.

`ItemVariant` in `src/core/Loadout.h` is this. The plain variant has every part empty. Wherever a variant is held it is optional, and no variant means **the form, whichever variant**: the combat AI's own list of options holds forms, and a rule may name a form without picking a row. `SameThing` is one form and the same variant, or no variant on either side; the enchantment matches effect for effect in any order (`SameEnchantment`), the numbers within a hair of what the save wrote.

**Rows follow the engine; variants are coarser.** The Inventory tab splits rows as the game's screen does: a list stands apart when it holds any entry but count, hotkey and the worn marks, which is the rule of the engine's own arrival comparison (11594), with worn copies folded into their stack as the menu folds them. A variant covers one or more rows: the clean stack and the poisoned dagger are one variant, the charged sword and the drained one are one variant, and a pin or ban on any of those rows is on all of them. Every row falls under exactly one variant, so whatever the inventory shows can be equipped by its own row while the pin behind it is satisfied by any row of the variant. Nothing is ever written onto an item.

**The count of a variant** is the sum over its rows. The one-copy rule -- no second copy for the other hand -- asks it: two daggers pinned one per hand need two copies of the variant, and a clean dagger is not a second copy of a tempered one.

## Where the variant goes

**The panel is exact; the rules and the engine have the leeway.** A click on a row means that row: the list clicked, or the plain stack's own copies. The pin or ban it makes is on the row's variant, since that is what can be promised over time, and the pin's marker sits on the copy worn where the pin says, the incumbent, whatever row it came from. Choosing among the copies of a variant is what a rule's equip, the watchdog and the engine's own pick do; the player never asks for "a" copy.

- **A pin** is a variant and a hand or slot. It is made from a row: the panel's click, or a rule's equip; a rule that picked no row pins the form, whichever variant. It is satisfied by any row of the variant worn there.
- **A ban** is a variant. Banning "Iron Dagger" bans the clean stack and the poisoned one, and both rows show it; "Iron Dagger" with an enchantment is another variant.
- **A rule's equip** names a row's variant, or the form alone, and reads as that row's display name while a row matches (refreshed from the bag each scan; `Action::name` keeps the last one seen while none does, and the rule is set aside as not carried). A rule keeps its variant whatever happens to the bag.
- **The save** carries the variant as one object beside the form on pins, bans and equip actions (`docs/PROFILES.md`), each part in it only when present and no object at all for the form alone; the enchantment goes as its list of effects, which is why a copy enchanted at the table survives a round trip through the file when its minted form's number would not. A pin is taken back only if a row of the variant is worn where the pin says; a ban if a row of the variant is carried; a rule as written.

## The three means, and what each sees

Nothing is written on the item or the follower; all three go away with the DLL. None of them chooses a copy: ours is to force a pin and prevent a ban, and the engine's tie-breaks are left to it.

**The incumbent first.** A copy of a pinned variant worn in a hand or slot is the incumbent there. An equip aimed at that place that names no list, or names a copy of the same variant, is a no-op to the pin's purpose and is refused, so the engine's own pick cannot displace the incumbent with a plain copy and set the watchdog flapping. An equip naming the incumbent's own list is the engine re-equipping what is there and passes. An equip of the same form into the **other** hand is not a no-op and goes on.

- **The equip detour** is handed the copy's list, or null for the form. With a list, it judges by that copy's variant: a banned variant is refused, another variant into a pinned place is refused, the pinned variant itself passes. With null -- every equip the combat AI makes -- it walks the bag as the engine's own equip does (below), leaves out the rows whose variants are banned, and hands the engine the list of the first row the engine would itself have reached; a plain pick with listless copies remaining needs no list named. With nothing left, the request goes on as it came.
- **The score hook** sees the form. For a pin it shadows by form, a superset. For a ban it zeroes the form only when every row of the form is banned and no pin holds the form; with one row allowed the form keeps its score and the detour picks the row, and a pinned banned thing -- a rule's fight-time override -- keeps its score, or the AI stands holding a weapon it will not swing.
- **The watchdog**, each half second: for each pin, if no row of the variant is worn where the pin says, it dresses one by the same walk, the engine's order minus the bans, restricted to the variant's rows, and hands the engine that row's list (null only for the plain variant while listless copies remain: the engine's own first step takes a listless copy, and every listless copy is plain; past that, null would let its second step take another variant). For each ban, a worn row of the variant comes off, unless a pin holds it, which is the fight-time override. **If no row of a pin's or ban's variant is in the bag, it is dropped.** What can still take a pinned thing out of a hand, past the detours: a disarm shout, a script's unequip (not detoured, on purpose), our own casts, which borrow the hand and give it back, the item leaving, and the rules' fight-time pins.

**The one selection rule.** The engine's equip, asked for a form with no list, takes a plain copy first (by its own per-list predicate, `IsInventoryStackable`, whose table calls tempering, charge, a poison and a name indifferent) and else the first list in the entry's order not already worn. `EnginePick` in core is that walk with banned variants left out, and the detour, the watchdog and the pin path all use it. The engine's tie-break, less the bans, and nothing of ours added.

## What a round trip does

A variant cannot be reissued to a different kind, so a copy that leaves and comes back unchanged is the same variant: a rule naming it is available again on return. A pin or ban, though, is **dropped the tick its variant has no row in the bag** -- a round trip longer than a tick, the smithing trip included, means pinning again on return. That is chosen: a waiting pin needs a home in the panel and a way to clear it, and rules already cover the durable case. Improving the item changes its variant and ends the pin, as agreed. Nothing needs an event: the bag is read each tick.

## What the unique id turned out to be, and why it is not the key

`ExtraUniqueID` is one more entry on a list, read from the running executable 2026-09-12. What the engine does with it is the engine's, whoever issued it; what it does not do is the reason it is not our key:

- **Issued to a list, never to a listless copy**, and by the engine only for what a quest tracks: vanilla mints none for ordinary gear. In Nordic Souls every gifted copy had one because a plugin there hooks the container-changed event (16149, a jump through a pointer outside the game image on that process; clean on the vanilla instance) and issues ids for whatever arrives. The plugin is not named yet.
- **The lowest number not in use in the bag**, from both generators (16147, 16148): with 1, 2, 3 present and 2 gone, the next arrival is 2. Two iron daggers given back in the other order swapped numbers. The number is per bag across every form, so a departing dagger's 10 is the next circlet's.
- **Cleared by the engine on leaving** (16059 through 16065, identical on both instances): no id survives a hand-over. A returning copy is a stranger to any record keyed by id.
- **Kept out of stacking only by convention**: the arrival comparison does not skip it, so a copy with an id and a like copy without one do not merge. Issuing ids on sight, which the branch did for a day, split returned plain copies into separate rows in the game's own screen as well as ours. Favourites are the type the engine does skip, which is why a favourited stack stays a stack and why a hotkey cannot name one copy.
- **The assigner exists**, `InventoryChanges::SetUniqueID(list, oldContainer, item)`: the lowest free number, set on the list with the bag's form as owner, the change event raised. ID **16147** in 1.6.1170 (SE 15907); CommonLibSSE-NG's wrapper maps it to 16149 on AE, which is `SendContainerChangedEvent`, and would assign nothing. Not used any more; recorded so it need not be re-read.

The id would have been exact where the variant is coarse -- two copies alike in every respect the variant reads are two ids and one variant -- and that exactness buys nothing a tactic wants, at the price of a key that dies on every hand-over and fragments the follower's stacks.

## The plan

Agreed 2026-09-12, replacing the id design built earlier the same day; 1 to 4 built on the branch the same day, 5 open:

1. **Core tells copies apart by variant** (`ItemVariant`, optional wherever it is held; none is the form): pins, bans, actions, the holdings, the profile. Tests around it: two rows of one variant are one pin; a tempered copy is another; the count is the variant's; `EnginePick` restricted to a variant.
2. **The scan reads the variant off each row's list**; rows split as before; the picker's leaves carry the row's variant and colour.
3. **The detour, the watchdog and the pin path** on the one selection rule with the incumbent check first; bans by variant; the watchdog drops a pin or ban whose variant has no row.
4. **Out**: the id key, issuing on sight, the leaving sink, the plain-copy sentinel.
5. **Verify in play**: a poisoned pinned dagger stays pinned through the dose and its wearing off; a stolen copy is its own row and variant; the AI under a partial ban draws the allowed row; a tempered copy pinned, taken to a grindstone and returned is a new variant.

## To verify in play

- **What a transfer leaves on a list.** Answered 2026-09-12, from the scan's debug line: the player's **ownership** (`ExtraOwnership`, `0x21`), one list per copy handed over. A tempered copy carries health, text display data and ownership (`25 99 21`); an outfit piece the outfit mark and the worn mark (`8E 16`); Jenassa's own bow and arrows an alias mark (`95`, `ExtraFromAlias`).
- **What the game's menu splits on.** Ownership it ignores (above); poison and tempering it splits. The outfit mark, the alias mark and a leveled-list mark are unread: our scan splits on them, the game's menu may not. `InventoryEntryData::NormalizeAndCountNonStackableExtraLists` and `ExtraDataList::IsInventoryStackable` are the candidates for its rule; the scan's debug line says, per list shape, whether the engine calls the list stackable, to be read against what the menu shows.
- **Does a leveled-list item have an id or a list?** Expected neither, as an outfit piece has no id.
- **The outfit mark on the enchanted armour.** Frea's enchanted Nordic Carved Armor showed the Outfit tick, read off its own list. Either it was her outfit piece before it was enchanted and handed back, or the engine marks every copy of an outfit form at load. Its history decides.
- **Does scoring read tempering or enchantment?** The AI's entry holds only the form; not read (`tools/livedisasm.py --vtable` on the melee entry's table, slot 0x0C).
- **Which plugin in Nordic Souls hooks the container-changed event** and issues ids: read the hook's target on that process and map it to a module.
