# The proof of concept — what was built, what is verified, what to try in play

**Folded into Follower Tactics on 2026-09-21** (branch `wip-progression`): the code is `src/progression/` (`core/` tested by `tests/progression/`, `game/` in the plugin), its records are in Tactics' co-save block, and a follower's own pages carry it: the skill page on their Skills tab, the experience bar on their Character tab, and the switch in the Progression section of Tactics' Settings page, *Manage follower progression*. Progression's own pages (an Overview, its Settings, a page per companion) were taken out the same day, before they were seen in play; what they held that the followers' pages do not yet -- attributes, setting their own perks and spells aside, teaching and forgetting -- comes back there as each is taken up. The stand-alone repository (`C:\project\SkyrimFollowerProgression`) keeps the history before that. The journal was taken out the same day: with no discoveries to record, nothing in it was needed.

Built 2026-09-21, overnight, from [PROGRESSION.md](PROGRESSION.md). The short version: **the rules are built and tested; the engine layer is built and has never run.** Nothing here has been loaded into Skyrim. Every claim about the game below is either read from records and source, or marked as a thing to check.

## What there is

| Part | Where | State |
|---|---|---|
| The design | [PROGRESSION.md](PROGRESSION.md) | Written; the pacing table is the simulator's output |
| The rules | `src/progression/core/` (`fp_core`) — no Skyrim, as Tactics' core | Built; `tests/progression/` (`fp_tests`) passes |
| The vanilla perk trees | `tests/progression/data/vanilla-perks.json`, from [research/extract_perk_trees.py](research/extract_perk_trees.py) | 180 nodes, 251 ranks, read from Skyrim.esm through houseCARL (read-only) |
| The engine layer | `src/progression/game/`, started from Tactics' `src/plugin.cpp` | Written against CommonLibSSE-NG 7.5.1; see "Build" below |
| The pages | Follower Tactics' own: the skill page (`src/game/UI.cpp`, `DrawPerkTree`), the experience bar, the Settings switch | Progression's own pages were written and taken out on 2026-09-21, never seen |
| A clickable mock-up of the panel | the stand-alone repository's `prototype/` | Not brought over: it predates learning by doing |
| Console scripts for the first session | `bat/fpsetup.txt`, `bat/fpcheck.txt` | Written; tome ids read from Skyrim.esm |

### The core, module by module

| Module | Decides | Tested by |
|---|---|---|
| `Levelling` | The player's levelling formulas, over whatever numbers the game has: skill XP from a use, a skill level's threshold, character XP per skill-up, a level's threshold | `test_levelling.cpp`: the level curve's known values, the formulas, a changed setting changing the answer |
| `Perks` | Each rank's own conditions evaluated (OR groups, the trailing OR vanilla writes, every comparison), implied lower ranks, the Learn/Unlearn verdict and why, what a lower skill invalidates | `test_perks.cpp` on a small tree; `test_vanilla.cpp` on all 180 real nodes, read by the tests' own `PerkData` |
| `Spells` | Whether a tome can be taught, and why not | `test_spells.cpp` |
| `Companion` | The ledger: a use practised, the level (the engine's, or more by what they learned, capped past the player), points as the difference, the reassigning pool and Reset, the engine's delta, perks, spells | `test_companion.cpp`: use to skill-up to character XP, the level's three cases, points, the pool down to the floor and up to the cap, a level a bought perk needs kept until a reset, idempotent application |
| `Serialize` | The co-save: one bounded JSON record per companion | `test_serialize.cpp`: round trip, unknown fields, a bad record costing only itself, a newer schema, duplicates |

A few things the tests established that were not obvious from the design:

- **Vanilla's prerequisites are in each perk's conditions, not in the tree's lines.** Magic Resistance is drawn from the root; its record requires Apprentice Alteration. Two parents are an OR group whose last member also carries the OR flag. Only two condition functions occur in the 18 trees: `GetBaseActorValue` (277) and `HasPerk` (448).
- **Companions' Path reads the wrong functions for requirements.** Its `GetSkillLevelRequirement` treats function ids 71 and 73 as actor-value reads; in the engine they are `GetInFaction` and `GetFactionRank`. It works on vanilla only because vanilla uses 277. (Added to [PRIOR_ART.md](PRIOR_ART.md).)
- **A follower's own perks need not be contiguous.** Marcurio's record holds Recovery's second rank without the first, and Magic Resistance without Apprentice Alteration. A held rank now implies the ranks below it; innate perks satisfy prerequisites whatever their own were.
- **Ranks stack.** A lower rank's entry switches itself off (`HasPerk <next rank> == 0`), so a companion holds every rank learned, as the player does.

### The engine layer, file by file

| File | Does |
|---|---|
| `Actors.cpp` | Who is a follower (teammate, person, not dismissed, alive), waiting, unique; skills and attributes as the engine gives them, without what they learned (the value view's engine base); the perks on a record (read only) |
| `PerkView.cpp` | The perk view: Character's `ForEachPerk` and `ApplyPerksFromBase` replaced, so the engine's `HasPerk` and its effect registration see a companion's record less the set-aside plus the bought; changes while loaded queued through the engine's own rank change ([ENGINE_PERKS.md](ENGINE_PERKS.md)) |
| `PerkTrees.cpp` | Reads every skill's tree off the engine at data load, conditions converted item by item, each node classified from its entry points; writes the graph out for comparison (Settings, "Write the perk trees") |
| `Tomes.cpp` | The player's spell tomes, a companion's known spells, teaching (the tome is taken only once `HasSpell` says yes), forgetting |
| `SpellView.cpp` | The spell view: `VisitSpells` detoured and Character's `CheckCast` replaced, so a companion knows what the engine gives them less their set-aside spells plus the ones taught here ([ENGINE_SPELLS.md](ENGINE_SPELLS.md)) |
| `Learning.cpp` | Where skill use comes from: Character's `UseSkill` replaced (magic), and the hit handler's call to the victim's processing hooked, the player-only weapon, Block and armour uses worked out the same way for companions ([ENGINE_SKILLS.md](ENGINE_SKILLS.md)) |
| `Rules.cpp` | The game's levelling numbers read now: the settings, and each skill's usage values through the engine's own reader |
| `Service.cpp` | The ledger's home: the paced tick (enrolment, reconciling what they have), each skill use, every action from a page, the views the pages read (rebuilt after each action and as a follower's page comes up), the co-save's contents |
| `Events.cpp` | The main menu's opening, and the thread that paces the tick (one task a second, never a task that queues itself) |
| `Persistence.cpp` | The co-save records, written and read inside Tactics' callbacks (`src/game/Profiles.cpp`), told apart from Tactics' own by type |

## Build

Tactics' build, and its checks (CLAUDE.md, "After every edit"): Progression's core is `fp_core`, its tests `fp_tests`, both run by the `core` presets' tests; the formatter and both tidy targets cover `src/progression`.

```powershell
.\tools\build.ps1 -Preset core -Test     # both cores and their tests: seconds
.\tools\build.ps1 -Preset debug          # the plugin, Progression in it
.\tools\package.ps1                      # dist\follower-tactics-<version>-test.zip
```

**Folded, 2026-09-21:** `core`, `core-asan` (445 tests, Progression's among them) and `debug` build and pass; the formatter and both tidy targets are clean over `src/core`, `src/game` and `src/progression`. Getting tidy through took `std::format` out of Progression's core (clang-tidy's analyser falls over inside MSVC 14.42's `<format>`, as Tactics' `core/CoSave.cpp` notes) and `fmt::format` in place of it in the game side's test reports.

What follows is the stand-alone build's record.

**Build status, 2026-09-21:** the `core` preset builds with no warnings and all 57 tests pass (under the fetched Catch2 and under vcpkg's); the `debug` and `release` presets build the plugin with no warnings; `tools\package.ps1` writes `dist\follower-progression-0.1.0-test.zip` (a 1 MB DLL exporting `SKSEPlugin_Load`, `SKSEPlugin_Query` and `SKSEPlugin_Version`, as Follower Tactics' does). The engine layer was reviewed line by line by a second agent against CommonLibSSE-NG's source and Follower Tactics' code before packaging. It found no definite crash; what it did find is fixed and listed below. None of that is a substitute for the session below.

What the review changed:

| Finding | Fix |
|---|---|
| Menu entries added while holding the slot lock that the render callbacks take: a possible lock inversion with the framework's own | Slots claimed under the lock, entries added after it is released, as Tactics does |
| The encounter touched from the death sink without a lock, while deaths may arrive on another thread | Its own mutex (order: `g_mutex`, then the encounter's) |
| A fight shorter than the one-second tick paid nothing: deaths before the tick saw combat were dropped | A death by the party, or while the player is in combat, begins the encounter itself; the killer is passed through |
| The SKSE revert callback, mid-load, removed perk entries from actors | Full removal at `kPreLoadGame` with the world whole; the revert callback touches base lists only |
| A dismissed companion's perks were not put back after a load until recruited again | Every enrolled companion with 3D loaded is reconciled each tick, follower or not |
| A cleared dungeon was found by searching the player's chain for any cleared link: could pay a child room cleared earlier, miss the parent, or pay one cleared before | The chain is recorded on entry with each link's cleared flag; what flips pays, from the event and, as a fallback, the tick |
| `worldLocMarker` taken as "has a map marker" | The handle resolved and the reference required to carry `ExtraMapMarker` |
| Page state reset from the framework's close event, whose thread is not promised | Reset on the next frame's `kBeforeRender` |
| Ticks piling up during a load | At most one queued (`g_tickQueued`, as Tactics) |
| A form injected into another plugin's range did not round-trip through its key | The plugin is taken from the id's load-order slot, not the first file to touch the form |
| Views rebuilt every second while any F1 page is open, reading unloaded actors | Rebuilt only when one of our pages was drawn; tomes scanned once; an away companion keeps their last reading |
| After *Quit to main menu*, the panel still acted on the game just left | The main menu's opening marks the game left |

A second pass over the fixes found one regression and three gaps, also fixed: the party-kill rule for foes had let a fleeing deer or bystander count (a foe is again only something hostile to the player; a party kill only begins a fight); the dungeon chain is now seeded at load and settled before a door replaces it; the shared tome list is priced with the player rather than a null caster; perk entries are only taken off actors that are loaded. And what counts as a *place* was checked against the records rather than assumed, because no single field says it: Bleak Falls Barrow's world location marker is its map marker; Riverwood's is a plain XMarker, but Riverwood lists its map marker among its special references as `MapMarkerRefType`, as Halldir's Cairn does; Whiterun has neither and carries `LocTypeCity`; Breezehome, Dragonsreach, the Bannered Mare and Jorrvaskr have centre markers of their own; Falkreath Hold has one and `LocTypeHold`. `CountsAsPlace` therefore takes a map marker (special reference or world marker) or a city or town keyword, and a house or hall inside a city counts as the city. The log line `entered … counts as …` in step 8 is how to see it choose.

The perk view ([ENGINE_PERKS.md](ENGINE_PERKS.md)) had a review of its own, which re-read the disassembly behind it and confirmed the slots, the visitor's convention, the rank change's signature and that `ApplyPerksFromBase` only queues. What it changed:

| Finding | Fix |
|---|---|
| Perks were taken off with each entry's own `RemovePerkEntry`: ahead of any add still queued, without the engine's event, and on entries of ranks never applied | The queued rank change's handler (23822) was traced: rank → 0 is supported, and is now what takes a perk off |
| A bought ability perk (Recovery, Magic Resistance) is an `AddSpell`, and the save keeps it: "nothing is written to the save" was wrong | Said so; turning progression off (Tactics' Settings) takes everything of ours off each companion, abilities included, keeping the ledger for turning it on again |
| The bookkeeping mutex was held across calls into the engine, which the process-building hook, called under engine locks, also takes | Worked out under the lock, queued after it |
| A change queued in the same frame as a process rebuild registers twice (the entry table keeps duplicates) | Documented and accepted: it needs both in one frame |
| Every NPC's `HasPerk` loaded the shared map (a spin lock and two reference counts) and bumped a shared counter | A small atomic array of managed ids checked first; only managed walks counted |
| Nothing stopped the hooks on VR, whose slots differ | `Install` does nothing on VR |

The spell view had its own review too. It confirmed against both lines: the detour's signature and the visitor's calling convention; that every caller of `VisitSpells` survives a skipped or an extra spell; the `CheckCast` slot; and `DeselectSpell`. What it changed:

| Finding | Fix |
|---|---|
| A forgotten or released taught spell could be cast for the rest of a fight, from an inventory gathered before, and stay in hand for the save | Withdrawn: `CheckCast` refuses it and the tick keeps it out of hand until the companion is out of combat |
| A UseMagic package keeps a spell it chose before it was set aside | The process's `currentPackageSpell` is cleared of a set-aside or withdrawn spell |
| Teach's rollback could erase an earlier ledger entry and pop an unrelated journal line; on VR it could leave the spell on the actor | An already-taught spell is refused up front; the ledger is put back from a copy; the VR path takes the spell back off |
| The counters counted our own `HasSpell` and `CheckCast` calls | The combat AI's gathers counted apart, by their visitor's vtable; the check's own questions not counted |
| Past 64 taught spells, one could be handed over twice | At most 64, said in the log |
| A set-aside spell dropped from the record by a mod update could not be restored | Still listed, with *Restore* |
| `RemoveSpell` was said to end a castable spell's effects | It does so only for abilities, diseases and addictions; corrected |

## The first session in play

Install `dist\follower-tactics-<version>-test.zip` from `tools\package.ps1` in Mod Organizer, and **untick the stand-alone Follower Progression mod**: both would hook the same places. `tools\deploy-tests.ps1` puts `bat\fpsetup.txt` and `bat\fpcheck.txt` in the game folder with Tactics' own. Use a disposable save. Progression's lines are in `FollowerTactics.log`, under `[progression]`, `[growth]`, `[perks]`, `[spells]`, `[party]`, `[ledger]` and `[progression-ui]`; the test zip logs at debug.

The order matters: each step is what the next relies on.

1. **It loads.** The log says the perk trees were read (`read 180 perks (251 ranks)` on vanilla; more with mods). Tactics' Settings has a Progression section with *Manage follower progression*, on.
2. **The trees are the vanilla trees.** A follower's Skills tab, a skill's name: its page draws the tree as the perk menu does, perk for perk, on an unmodded order.
3. **Enrolment.** Recruit Lydia (or any unique follower). Within a second the log says *enrolled at level N*, and their Character tab has the gold experience bar beside the level.
4. **Learning by doing, read and not written.** Select them in the console, `bat fpcheck`, note the values. Fight something with them (Tactics' `bat ftspawn` places a draugr). After enough, `One-Handed` rises by one on their Skills tab and the log says *increased to*; with a caster companion, from magic too. `bat fpcheck`: One-Handed's base reads one higher, and its permanent modifier is still 0 -- the value view, not the actor. Then their Character tab: a click on *Health* opens `<<` `-` `+` `>>` after the bars; `+` spends a point, the bar's maximum and `getav health` rise by 10, the permanent modifier stays 0.
5. **A perk, and its effect.** One-Handed's page, a click on Armsman: the circle fills and the perk menu's sound plays; `hasperk 000BABE4` is 1 (the console's `hasperk` is the engine's `HasPerk`, which is the hooked `ForEachPerk`). The DESIGN.md P0 is the *effect*: compare their one-handed damage before and after (Follower Tactics' Character tab builds the hands' damage from `ForEachPerkEntry`, each Mod Attack Damage entry a line). A right click returns it: damage goes back, `hasperk` is 0.
6. **Save, load, restart.** Save; load it; `bat fpcheck`: the values read as before, the permanent modifiers still 0, `hasperk` still answers from the view, and the skill page draws the same. Quit to desktop, relaunch, load: the same.
7. **Save A / save B.** Make save B *before* step 3 and save A after step 5. Load A, then B: in B, `hasperk 000BABE4` on Lydia must be 0. With the view this should hold by construction (nothing is written to the record); this step is where that is confirmed. Then, on a copy of save A, buy them Recovery first if a Restoration companion is to hand (an ability perk), and turn progression off (Tactics' Settings): the skill pages show their record's perks, `bat fpcheck` shows the values as their record makes them, `hasspell` on Recovery's ability is 0. Save, untick the mod, load: the same, and nothing in the log about missing records. Then the point of the value view: on a copy of save A with progression *on*, untick the mod and load -- skills and attributes as their record makes them, with no step before. Turning progression on again, on the save made before unticking, puts it all back.
8. **Levels.** As they learn, the level rises (at most 5 past yours) and the log and a notification say *Lydia reached level N: … to assign*; the skill page's header has the perks to spend, the Character tab's attribute controls the points. A follower the engine caps (Onmund at 30) with you past 30: their level climbs above 30 only by what they learn.
9. **Moving skills.** With Armsman learned, `-` on One-Handed down to its requirement: it greys there and its hover says *Armsman needs this skill level*. *Reset perks* returns Armsman; `<<` then takes One-Handed to 15 plus the race's bonus. `+` on Two-Handed spends what that returned, a level at a time, and greys with *Not enough XP* when it runs out. `bat fpcheck` follows each click.
10. **Learning and forgetting.** `bat fpsetup`, then give Lydia the Flames tome and open it on her Inventory tab: the name is blue, *Learn* at the right says *Click to learn Flames*; Confirm, and the game's spell-learned sound plays, the tome is gone, the page is back on her books, and `hasspell 00012FCD` on her is 1 (the console's `hasspell` is the engine's `HasSpell`, which walks the detoured `VisitSpells`). Her Magic tab lists Flames, and a rule can name it. The P0 here is the **cast**: Lydia, taught Flames and nothing else, should use it in the next fight. It is known only through the view, never added to her. Pin it to a hand, then Forget it on its page: it leaves the list, the pin goes, a rule naming it reads *does not know that spell*, `hasspell` is 0. A second tome teaches it again. Then Marcurio: Forget his own Sparks, and `hasspell 0002B96B` on him is 0 and he doesn't cast it in a fight; a Sparks tome brings it back.

Anything that fails is a finding, not a bug to route around: most of steps 4–7 are the P0 experiments DESIGN.md asks for, and this build is the harness for them.

## What is deliberately not here

Dialogue (needs an ESP), owning engine level, setting aside abilities, non-unique followers, creatures, the Tactics interface for naming assigned points in its breakdowns, quest and travel experience, restoring a companion to how they were before enrolment. [PROGRESSION.md](PROGRESSION.md) has each in its place.

## Known risks in this build

- **Base perk lists: no longer edited.** The first build put bought perks on the shared base record; the view replaces that (`src/progression/game/PerkView.cpp`, [ENGINE_PERKS.md](ENGINE_PERKS.md)). What it depends on instead: two vtable slots and one engine function read from 1.5.97, 1.6.1170 and 1.7.104, none yet exercised. Not installed on VR.
- **The value view.** One vtable slot, read on 1.5.97, 1.6.1170 and 1.7.104, not yet run. A reader that goes to an actor's value storage directly, not through its value interface, would miss what they learned; and the engine's recalculation of an NPC's values as they level is not yet read -- if it writes back a value it read, the learned levels would land on the actor ([ENGINE_SKILLS.md](ENGINE_SKILLS.md), "Read, not written"). Step 4's `getavinfo` shows either.
- **Threads.** The panel reads a copy made under a lock; every change is queued to the game thread. The pacing thread and the learning hooks only queue.
- **Learning's rate.** A companion's uses are worked out by the player's own rules, but a follower fights all the time and takes most of the blows; their weapon, armour and Block skills may climb faster than a player's would ([ENGINE_SKILLS.md](ENGINE_SKILLS.md), "Not verified").
- **Attribute points from their own values.** An NPC's health, magicka and stamina come from their class; counting what they carry above their race's start as spent may leave some with no points. The first session should log a few.
- **The HUD.** Notifications use `SendHUDMessage::ShowHUDMessage`; several levels from one gift are one line, but several companions levelling at once still queue several.
- **Ability perks.** A perk that works by adding an ability (Magic Resistance, Recovery) is an `AddSpell` on the actor, and the save keeps it: uninstalling without turning progression off first leaves a bought ability behind, the one thing of ours a save keeps. Step 7 checks that releasing takes it off.
- **Plugins that read the record's list.** SKSE's `ActorBase.GetNthPerk` and `GetNthSpell`, Papyrus extenders' perk lists, Follower Tactics' Skills tab and its spell walk: they see the record and the actor's own lists, not the views. Tactics' spell walk goes through `VisitSpells` since 2026-09-21 (confirmed in play with a taught Sparks).
- **Spell view gaps.** A fight's combat inventory, gathered before a spell was set aside, may still cast it until the fight ends. A set-aside spell stays hidden even if a quest grants it again ([ENGINE_SPELLS.md](ENGINE_SPELLS.md), "Not verified").
