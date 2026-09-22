# How the engine answers an NPC's perks, and the view we put in front of it

Read 2026-09-21 from the unpacked executables in `C:\Modding\SkyrimVersions` (1.5.97, 1.6.1170, 1.7.104) with `tools/disasm.py`, which is Follower Tactics' disassembler (its `tools/skyrimexe.py` and `addrlib.py`) copied here. Everything below is read from code, not seen in play. The plugin's use of it is `src/progression/game/PerkView.cpp`; the address pairs are in `src/game/Addresses.h`.

## Why

An NPC's perks are a list on the base NPC record, shared by every copy of that NPC. The engine gives NPCs no other perk storage of their own: `Character::AddPerk` and `RemovePerk` are both a bare `ret` (1.6.1170 IDs 37694, 37695). So the prior art, and this project's first build, edited that shared list: in memory, process-wide, needing to be undone before another save loaded, and restricted to unique NPCs. Reading how the engine *asks* about perks showed a narrower route: answer the questions differently for the actors we manage, and leave the record alone.

## What the engine does

| Question | 1.6.1170 (AE) | 1.5.97 (SE) | What it reads |
|---|---|---|---|
| Which perks does this NPC hold? `Character::ForEachPerk`, vtable slot 0xFA | 37693 | 36685 | The base NPC's perk list (`[actor+0x40]` + 0x138, the `BGSPerkRankArray`), through the list's own walk (14320 / 14211) |
| Does this actor have perk P? `Actor::HasPerk` | 37698 → 40021 | 36690 → 38962 | The actor's process first (`[actor+0xF8]` on AE, `+0xF0` on SE; no high-process data, no perks); then **the actor's own `ForEachPerk` virtual** (`call [rax+0x7D0]`) with a finder visitor (vtable 207861, operator 23827), reading the rank through the pointer the finder kept |
| Are there effects on entry point E? `HasPerkEntries`, slot 0xFF | 37701 → 40024 | 36692 | The high-process data's table of 0x5C entry points (`[middleHigh+0x288]`, 24 bytes each) |
| Visit them. `ForEachPerkEntry`, slot 0x100 | 37702 → 40025 | 36693 | The same table. Shared by NPCs and the player: both classes' vtables hold the same function |
| Register a new process's perks. `ApplyPerksFromBase`, slot 0x101 | 37703 → 40026 | 36694 → 38966 | The base NPC's list, walked **directly** (14320, not through `ForEachPerk`), queueing each perk's rank change 0 → its rank (visitor 207863, operator 23829) |
| Change a perk's rank on an actor | `TaskQueueInterface` (singleton 403759), 36982 | singleton 517228, 36007 | Queues a task (type 0x5F) of actor handle, perk, old rank, new rank, under the engine's recursive spin lock (36993); run at once on the calling thread when the queue is switched off, otherwise drained on the main thread (36891). Also what the player's `AddPerk` calls (40770 → 36982 with 0 → rank). SE's 36007 is an exact shape match of AE's 36982 |
| Carry that change out | 23822, from the task executor's case 0x5F | | For each of the perk's entries: removed (entry slot 0xB) if its rank + 1 is the old rank, applied (slot 0xA) if it is the new one; then an event is sent (singleton 401348). So 0 → rank adds a perk and rank → 0 takes it off |
| An ability entry (Magic Resistance, Recovery) | 23796 / 23797 (`BGSAbilityPerkEntry`, vtable 195297) | | Apply is `Actor::AddSpell`, remove is `Actor::RemoveSpell` (38716 / 38717): the ability goes in the actor's added spells, **which the save keeps** |

1.7.104 has the same IDs and the same vtable slots as 1.6.1170 (checked for 37693, 37703 and 36982).

Three consequences:

1. **`HasPerk` is `ForEachPerk`.** Replacing the one slot changes every `HasPerk` about an NPC: condition functions (perk requirements, spells that check a perk such as Impact and Deep Freeze, a rank's own `HasPerk <next rank> == 0` switch), and Papyrus.
2. **Effects are on the process, not the record.** Damage, cost and armour ask `ForEachPerkEntry`, which never looks at the record. What is registered there is whatever `ApplyPerksFromBase` queued when the process was built, plus later changes.
3. **Only two functions walk the record.** A reference search for the list's walk (14320 on AE) finds exactly two callers: `Character::ForEachPerk` and the process's `ApplyPerksFromBase`. The direct callers of those two are the player's overrides (40680, 40686), which call the Character versions by address, so patching Character's vtable leaves the player exactly as it was. What the search cannot find is code that reads the list's fields without the walk; none turned up, and none was looked for exhaustively.

## The view

For each managed companion, by the reference's runtime id: **the record's perks, less the set-aside ones, plus the ones bought here.**

- **Slot 0xFA, `ForEachPerk`.** For a managed actor, walk the record's list skipping the set-aside, then the added. For everyone else, the original. The visitor's first slot takes a `PerkRankData*` and returns 1 to go on and 0 to stop (read from the finder, 23827). The finder keeps the pointer past the walk and reads the rank through it, so added perks are handed out from a pool of `{perk, 1}` entries that is never freed.
- **Slot 0x101, `ApplyPerksFromBase`.** For a managed actor with high-process data, queue 0 → rank for each perk of the view, exactly as the original does for the record, and remember what was queued. For everyone else, the original.
- **Changes while loaded** (learn, unlearn, set aside, restore, retrain) are reconciled against what was queued, through the same queued rank change: 0 → rank for what's missing, rank → 0 for what should go. Going through the queue keeps a removal behind any add queued before it; each entry's own `RemovePerkEntry`, as the prior art does it, jumps the queue, skips the event, and removes entries of ranks never applied.
- **The view is published whole.** It's rebuilt from the ledger after every change and swapped atomically, because `HasPerk` is asked from combat and animation threads.
- **Unmanaged actors pay almost nothing.** Every NPC's `HasPerk` goes through the hook. Loading the published map is, in MSVC's library, a spin lock and two reference counts; so the managed actors' ids are also kept as a small array of atomics, checked first. Past 128 managed actors the check is switched off and every call loads the map. Only walks answered from a view are counted.
- **No lock is held across the engine.** What to queue is worked out under the bookkeeping mutex, and queued after it is released: the queue can run a task at once, and the process-building callers hold engine locks of their own.
- **The record is never written, and the view is not saved.** Before a load the views are dropped; after it they are published again from the save's ledger. What *is* saved is an ability perk's spell (above): a bought Recovery stays in the actor's added spells. Turning levelling off (Tactics' Settings) is for that: it publishes empty views, queues each companion back to their record, drops bought perks' abilities directly as well, and withdraws the training, companion by companion as each is near. The ledger is kept, and turning levelling on puts it back. A set-aside ability perk of their own needs nothing: the engine's `ApplyPerksFromBase` adds it back.
- **Not on VR.** Its vtable slots differ and weren't read; `Install` does nothing there and perks stay as the records have them.

What this buys over editing the record: no cross-save leak, nothing left behind on uninstall, perks a follower came with can be set aside and restored for free, and two copies of one NPC can differ. The unique-only rule remains in `IsUniqueNpc` for enrolment for now; the view no longer needs it.

## Not verified

- **That it runs at all.** The slots, IDs and calling conventions are read, not exercised. The skill page's circles are filled by what the engine's own `HasPerk` answers, which is `ForEachPerk` with a finder: a perk acquired there and drawn filled is the view answering, and the console's `hasperk` asks the same. The check against the view that Progression's own Settings page had went with those pages (2026-09-21).
- **Timing at load.** Whether the co-save is read, and the view published, before a companion's process is built during a load. If not, the tick's reconcile registers the difference a second later; the check above would show a brief disagreement.
- **Ability perks and turning levelling off.** That the ability is in the save is read from code (23796 → `AddSpell`); that releasing takes it back off, and that a save made after releasing loads clean without the plugin, is to be seen.
- **Threads.** `ApplyPerksFromBase` may run off the main thread. The hook only queues, as the original does, and takes a mutex only around its bookkeeping.
- **A perk registered twice around a rebuild.** The entry table's insert (40022) keeps duplicates. If a change is queued (0 → rank) and, before the queue runs, the process is rebuilt (40027 clears the table, then the hook queues the whole view), both adds apply, and the effect is doubled until the next rebuild. It needs a change to the view and a process rebuild in the same frame; vanilla never changes a record's perks at run time, so never meets it. Accepted for now.
- **Anything reading the record's list directly** would see the record, not the view. Follower Tactics' Skills tab is one: it would list a set-aside perk and miss a bought one. It should ask `HasPerk`, or use the interface in [PROGRESSION.md](PROGRESSION.md).

## Reproducing

```powershell
pip install capstone pefile
# Address Library databases, not committed: copy Follower Tactics' AddressLibrary\SKSE\Plugins\
# version-1-5-97-0.bin, versionlib-1-6-1170-0.bin, versionlib-1-7-104-0.bin into AddressLibrary\SKSE\Plugins\ here.
python tools/disasm.py --version 1.6.1170 --vtable 207886 260     # Character's vtable (SE: 261397)
python tools/disasm.py --version 1.6.1170 37698                   # HasPerk
python tools/disasm.py --version 1.6.1170 --refs 14320            # every walk of a record's perk list
python tools/disasm.py --version 1.5.97 --match 1.6.1170 36982    # the SE twin of the rank change
python tools/disasm.py --version 1.6.1170 23822                   # what the queued change does, entry by entry
```

The executables must be Steamless-unpacked copies (the installed one's code is encrypted on disk), as `C:\Modding\SkyrimVersions` holds.
