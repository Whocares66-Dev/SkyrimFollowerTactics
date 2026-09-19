# Testing

## The game layer without Skyrim

An assessment of what in `src/game` can be tested without the game was written on 2026-09-19 (commit d926e57 has it in full) and acted on the same day on `wip-game-test`. Its thesis holds and is now the way things are done: a decision the game makes from facts it has already read moves into core over a plain description of those facts, the game keeps the reads and the engine calls, and the fast presets test the decision. Each extraction switched the game over in the same commit, with no behaviour change unless the commit says so.

What was built, each in core with a test file of its own:

- `core/Clock.h` (`GameClock`): the calendar's hour and timescale into the seconds every cooldown, lease and hit window is measured on. `TacticsSeconds` reads the calendar and hands the numbers over. The tests pin what the hour alone cannot tell -- exactly a day is no time, an hour set back reads as the next day's -- as accepted.
- `core/Tick.h` (`ActorTick`): which list a tick evaluates for an actor and whether it is the fight's first evaluation or the farewell one, over what the tick reads (fighting, held, each list's switch, whether the idle list has rules). Writing the cases found a defect: a list switched off was still owed the fight's edges, so a follower with the combat list off never ran the idle list again after a fight. A switched-off list now spends the edge and drops its own sequence.
- `core/CoSave.h`: the framing of the co-save's records and the reading of a loaded set (unknown record, newer schema, cut short, the later of two under one key). A string's length was taken at its word and the string sized to it; a length past the record's end is now refused, and a record over sixteen megabytes is not read at all.
- `core/LogSettings.h`: the ini's text into the level and the events switch. The note for a value that does not read now names what stands rather than claiming the default.
- `core/BagView.h`: `dev/GAME_MODEL.md` part one, step one. One form's copies as the game reads them off the actor's entry -- variant, the engine's stackability verdict, the raw worn marks, count, the list as a token -- and over it the listless remainder, the marks read by the object's kind, the variant count, the worn and unworn rows of a variant and of the plain stack, the tab's rows, the engine's view for `EnginePick`, and `PlanEquip`, which copy an equip takes for the row clicked, the plain stack or the variant. `EquipPinned`'s list-choosing is the plan; the scenarios of 2026-09-12 are in `tests/test_bag.cpp` by number where a decision over the view was the bug.

What of the assessment was checked and held: `RowOfItsOwn` delegates to the engine's `IsInventoryStackable`; `ReadString` resized from an untrusted length; the ini's diagnostics named the default whatever stood; the clock is hour-only and reads a backwards hour as midnight; the sensor file is 4,859 lines; every function number it cites appears in the research documents it names. Its order of work is right; the four smaller extractions went first because each was an afternoon and the bag view the largest.

Still to extract, in the shape `ActorTick` has -- a state, a struct of what was read, a plan out, the game calling the engine from the plan: the follower lease state machine (`Packages.cpp`, `TickPackages` and `TickWeaponSlot`), the player cast state machine (`PlayerCast.cpp`, `Advance`), the bash transitions (`Blows.cpp`), and snapshot assembly (`Sensors.cpp`, `BuildSnapshot`, one family at a time). Then `GAME_MODEL.md` part two, the fake engine and the eleven scenarios end to end, folding in the watchdog simulation of `test_loadout.cpp`.

Judged not worth extracting yet: the hit sinks' classification (`Hits.cpp` is fifty lines of engine reads over the tested `HitTable`); the poison dose and recharge arithmetic (two lines each, the rest engine calls); the settings owner. The panel's cell readers, the click's next request, the sort and the filters in `UI.cpp` are free of ImGui calls and could be tested (a review of 2026-09-19 made the point; "ImGui-bound" was too broad), but each is a few lines over row types that live in game headers, so they are low on the list rather than off it. The assessment's eight reverse-engineering questions are research, not tests, and stay where `dev/UNIQUE.md` and `dev/TODO.md` keep them. Its harness advice is followed as written: Catch2 only, no fake `RE::Actor`, no vtable patching, no sleeps; every timer test supplies `now`, every bag is built by hand.

Found by the review of 2026-09-19 and fixed the same day: the tick handed an idle list's leftover sequence to a combat list switched on mid-fight (`core/Tick.h`, now dropped on the handoff, tested through the evaluator); the rules page wrote a renamed copy of the list back whole after building a snapshot, over any edit the panel made meanwhile (now renamed in place under the lock, the rename a tested core function, `RefreshActionNames`); the bag step of the snapshot repeated the spell step's scrolls, spells and soul gems. The review's design notes still open: capture an actor's two rule lists once per tick so pricing and evaluation read one version; carry the panel's request as a value with the copy question (`EquipAsk`) rather than an origin flag; the pin watchdog's fight lifecycle (`NoteFight`, `EnforcePins`) and `MarkPins` as transitions over the books.

What these tests do not establish: the adapters. `ViewBag` reading the entry, `ReadTick` reading the switches, the serialization calls, the calendar read and the file read are verified only in play, and none of the five extractions has been run in game since the switch (2026-09-19). The coverage report (`core-cov -Coverage`) now includes the new files; it says nothing about `src/game`, as before.

## Identity across time: what a token, an id and a request are good for

The rule for every handle that crosses from one tick to another, written down so a test can hold the adapters to it (the review of 2026-09-19 asked for this before the lease work).

- **A row token** (`BagRow::token`, the list's address) is good for the bag it was read from and for a later bag of the same actor only after `BagView::RowOfToken` finds it again. Finding it proves the address is a list in the bag now; it does not prove it is the same copy, since the engine reuses addresses. That is accepted: the panel's click is confirmed against what the row shows, and a wrong match equips a copy of the same form that happens to be at that address, which the watchdog then judges by the book. An exact-row request never substitutes another copy (`PlanEquip`, `EquipAsk::Row`); a variant request may.
- **An actor id** (`ActorId`, the form id) names the actor within one session of the loaded world. After a load, `ForgetSession` drops every per-actor state that holds one, and a package lease's handle is abandoned, not released, because the handle may resolve to an unrelated actor in the new world (`SlotLease::Abandon`). Nothing keeps an `RE::Actor *` across a tick; every tick looks the actor up again.
- **A panel request** (`RequestWear`, queued by actor id, form id, variant and row token to the game thread) is resolved on the game thread against the bag then: the actor looked up, the form looked up, the token matched. A request queued before a load is answered against the new world by those same lookups, which is wrong in principle and harmless in practice (the click is the player's, moments old). A session number on requests would make it right; not done, since no case has shown it.
- **A cooldown restart** is keyed by the action in flight and the actor no longer being busy (`FollowerState::inFlight`, `IsMidCast`, `IsMidBash`), not by a request id. Two requests cannot be in flight for one actor at once, so the inference holds today. A request id becomes necessary the day one can.

## Ledger: the review's backlog, and where each item stands

A review of 2026-09-19 (kept locally as `dev/REVIEW.md`, a scratchpad outside the tree) listed what else in `src/game` decides from facts already read. This is the working list, one line each, updated as items land; the commit named is where. An item is done when the decision is in core with tests and the game calls it, or when the review's design note is applied. Nothing here is verified in play until `dev/TODO.md` says so.

Simplifications (fewer owners first):

- [x] Duplicate snapshot producers in `FillBag` -- `160c921`.
- [x] Rules page renaming in place under the lock -- `0d1b1dd`.
- [x] Capture an actor's two rule lists once per tick (`ActorRules`, core/Tick.h); `SpellsNamedBy` a pure function of them, each spell once; one version for pricing and evaluation.
- [x] The wear request's origin as `By::Player` or `By::Rule` in `Pins.cpp`, the copy question (`EquipAsk`) derived from it in one place, Equip and Pin sharing `PutOn`. Not a request struct: the five callers pass six arguments each, and a struct would only rename them.
- [x] The meaning of each `ActionResult` per action, written on the enum (`game/Actions.h`).

Transitions (state in, plan out):

- [x] The tick's choice of list and edges (`core/Tick.h`) -- `53f1401`, `c1c7784`.
- [x] Follower spell and voice lease: `LeaseState`, `AdvanceCast` (core/Lease.h); the tick reads the engine and the sink's flags into a `LeaseSeen`, acts on the step. Cleanup order stays in `Release`.
- [x] Weapon lease: `AdvanceWeapon`. The "their own swing at arm time" reading stays in the game (it compares attack-data pointers) and arrives as one flag.
- [x] Player cast: `CastState`, `AdvancePlayerCast` (core/PlayerCast.h), every step, window and reason, the commands through a callback the test records. Still in the game, engine reads through and through: `ChooseHand`, `Lend`, `Restore`, `PlayerHeld`, `FireSeen`'s capture.
- [x] Bash and block: `BashState`, `AdvanceBash` (core/Bash.h). The two actions answer at once, so the step performs them through a callback the test scripts; the game's `Advance` reads the actor, performs, and captures the attack event on the first bash seen.
- [x] A coordinator harness (`tests/test_coordinator.cpp`): the tick as the game runs it per actor over the production planner, evaluator and cooldown restart, the action's result scripted and the busy capability set as `RuntimeCapabilities` sets it. (A first version left the capabilities out and reported a refire the game never makes; corrected the same day.)
- [x] The pin watchdog's fight lifecycle: `FightBook` (core/Watchdog.h) remembers, mirrors the panel's word and settles; `JudgePin` and `JudgeBan` answer each tick over what the game reads. `EnforcePins` reads, judges, reports and defers the equips as before.

Snapshot assembly (one family at a time):

- [x] Party and corpses: `AssembleParty` (core/Party.h) over the facts one walk reads of each loaded actor; the game builds the views for the ids chosen, in the plan's order. The two walks became one.
- [ ] Spells and effects (scrolls known when carried, wrapper shouts, no unlocked word, powers used today, instant and expired effects, only named spells priced).
- [ ] Buffs, consumable effects, applicability.
- [ ] Stats, reach and body radius.
- [ ] Hands and attack plans (`DescribeHands`, `PlanPowerAttack`, `PlanBash`, `DualWieldAllowed`).
- [ ] Poison and recharge planning (`WeaponToPoison`, `ChargeWeapon`).
- [ ] Hit sinks' classification into the tested `HitTable`.

Inventory beyond the view:

- [x] The bag view and `PlanEquip` -- `7161001`.
- [x] `MarkPins`: which row a ban marks (`BansRow`), which row a pin marks (`PinOfRow`, the worn incumbent or every row of the variant while none is), why a row is set aside (`RowAsideOf`) -- core/Marks.h, tested; the panel reads its rows and writes the marks.
- [x] `ScanInventory` draws the rows `DisplayRows` (core/BagView.h) gives it, over the view built from the entry it already holds (`ViewOf`); `RowsOf` is the same partition's variants. One algorithm for what a row is.
- [x] The identity and freshness contract: written above ("Identity across time").

The panel's decisions:

- [ ] Cell readers, the click's next request, `CellRank`.
- [x] The sort's order and the filter's matching (`core/Table.h`: `SortRows`, `Compare`, `ContainsNoCase`, `AnyContains`); the panel reads the sort spec and composes each list's cells, which stay with its row types.
- [ ] Open-row state across a move or a delete.
- [ ] Source navigation (`SourcePage`, `ItemPageOf`).
- [ ] `SyncFollowers` with its clock and pending set made explicit.

Other seams:

- [ ] `FindStack` destination choice.
- [ ] Roster cleanup in `Tick` (away, dismissed, dead).
- [ ] `ReportVerdicts` over `VerdictChanges`.
- [ ] Profile lifecycle (`ClaimSaved`, `IdentifyFollower`, carry-forward, revert).
- [ ] Log emit and archive with temporary directories.
- [ ] Custom skills loading and tree assembly.
- [ ] Remaining-time and description helpers (`Magic.cpp`, `Inventory.cpp`).

Not planned: a `GameAdapter` class. The discipline it names is followed; the recording backend arrives with the coordinator harness, not before.

## Existing console harness

The Phase 1 scenario — player + follower + hostile monster, follower drinks a potion at low
health — needs **no Creation Kit work at all**. It's four console batch files.

Files live in `bat/` (the console batch files; `tests/` is the Catch2 suite). Copy them to the **Skyrim root**, next to `SkyrimSE.exe`.
(Two sources say root, one says `Data\`. Try root first; if `bat` reports it can't find the
file, move them to `Data\`.)

## Why no Creation Kit

Three reasons a custom test cell isn't worth building:

1. Every actor can be spawned or summoned from the console at runtime.
2. A custom cell has to be maintained in an ESP, which then gets baked into your dev saves —
   exactly the churn Phase 0 is trying to avoid.
3. The follower is a **spawned copy**, created with `player.placeatme` from Lydia's
   **BaseID** `000A2C8E` — not the real Lydia.

### Correction: do not build this on Lydia's persistent reference

An earlier version of this harness used `prid 000A2C94` (her RefID) on the stated grounds
that a persistent placed reference targets her deterministically from a batch file. **That
is wrong before she is initialized**, and it failed exactly that way on a level-1 test
character: `prid` selected nothing, so every command after it silently did nothing, while
`player.placeatme` on the draugr in the same file worked fine and made it look like a
partial success.

> As long as you have not entered Breezehome, Lydia will not be initialized, so `prid` and
> `moveto` do not work on her.

She is only initialized after you are Thane of Whiterun. So the RefID is useless on a fresh
character — which is precisely the character you want for a clean dev save.

Two things follow, and both are worth internalising because they generalise:

- **`placeatme` takes a BaseID, not a RefID.** `000A2C94` names an existing *instance*
  (the thing that does not exist yet); `000A2C8E` is the *template* to instantiate.
  `player.placeatme 000A2C94 1` fails for this reason and is an easy mistake to make.
- **Spawned copies are better for testing anyway.** They exist at any point in any save, in
  any cell, regardless of quest state, and they are disposable. Nothing in the world
  references them, so `ftclean` can delete one outright.

The cost is one manual step: there is no console command that selects the reference
`placeatme` just created, so you click the spawned NPC in the console before the scripts
that configure her. Every script after `ftsetup` therefore operates on **the current
console selection** rather than a hardcoded RefID — which also means they work on any
follower you click, not just this one.

## Recruiting: dialogue, not the console (cast rules need it)

`ftmake` used to run `setplayerteammate 1` and `addtofaction CurrentFollowerFaction`.
That produces a *teammate*: the tick picks her up, potion rules work, and the panel
shows her. It does **not** fill the `DialogueFollower` quest's Follower alias, and the
alias is what carries the combat-override package list the cast route uses
(`dev/MAGIC.md`, "The eighth attempt"). So after `bat ftmake`, **talk to her and choose
"Follow me."** The log line `in the DialogueFollower alias: yes` on the first cast is
the check. Dismiss her through dialogue too; `ftclean` only undoes the console side.

A console route that should do the same, **unverified on this build**: after `placeatme`
prints the RefID, `cqf DialogueFollower SetFollower <refid>` calls the quest's own Papyrus
function, which is what the dialogue calls. It takes the ID as an argument, so it does not
suffer from the batch-file selection problem. The alias log line is the check either way.

Editing a file in `bat/` changes nothing until `tools\deploy-tests.ps1` is run again: the
game reads the copies in the Skyrim root. A stale copy is invisible in the log except as
`in the DialogueFollower alias: NO` on a follower who was "set up correctly".

## The rule that governs every script here

**Batch console commands are queued and executed in an order you cannot rely on.**
They are not run one at a time with each finishing before the next begins.

Everything else on this page follows from that, and it is worth stating first because it is
undocumented, invisible, and produces failures that look like something else entirely.

What it permits, and what it forbids:

| | |
|---|---|
| **Safe** | Lines that are independent of one another |
| **Safe** | Lines acting on a selection made *before* the batch ran |
| **Safe** | Explicitly prefixed commands — `player.additem`, `player.placeatme` |
| **Unsafe** | Any line depending on an earlier line in the same file having happened |

So `prid <ref>` followed by `moveto player` does not work: the `moveto` may run first,
against whatever was selected before, or nothing. Neither does printing a value before and
after changing it, or `getitemcount` after `additem` — the read can execute before the write.

**A separate `bat` invocation IS ordered** relative to the previous one. That is the escape
hatch: split a dependent sequence across two commands you type in order, rather than two
lines in one file.

### What this cost, and the misdiagnosis it caused

The first version of this harness used `prid 000A2C94` to select Lydia and then configured
her. It did nothing. The cause was diagnosed as her reference being uninitialised — true, as
it happens, since she is disabled until you are Thane — and a whole BaseID-and-`placeatme`
approach was built on that diagnosis.

That diagnosis was at best half right. The same pattern failed later on Marcurio, whose
reference was definitely live, because `prid` in a batch file cannot be relied on to run
before the lines that depend on it. The `placeatme` workaround appeared to succeed only
because `player.placeatme` is explicitly prefixed and needs no selection at all.

The tell was there and went unread: every command that ever worked from a batch file in this
project was `player.`-prefixed. Not one selection-dependent command was ever confirmed to
work, and that was treated as coincidence rather than evidence.

## Usage

One-off per playthrough, so the followers' references exist at all:

```
coc RiftenBeeandBarb          <- Marcurio   (verify names with console autocomplete)
coc WhiterunDrunkenHuntsman   <- Jenassa
coc QASmoke
```

Then, per follower — **select by hand, batch the rest**:

```
prid 000B9986      <- typed, not batched. Marcurio.  Jenassa is 000E1BA9.
bat ftmake         <- configures whoever is selected
```

Repeat for the second follower. Then:

```
bat ftbear         <- a cave bear; it can drive them under a threshold on its own
bat ftman          <- a level-1 bandit: a HUMAN enemy, for powers that only read people
bat ftstatus       <- read the selected follower's state
bat fthurt         <- force the threshold directly, if the bear is not obliging
bat ftclean        <- restore and un-follow. Non-destructive; safe on a real NPC
```

| Script | Acts on | Does |
|---|---|---|
| `ftmake` | selection | the relationship rank, three each of a health, magicka and stamina potion, the poisons, food, soul gems, spells, and gear including three iron daggers and three iron ingots for the copy tests. Contains **no `prid`** — that is the point |
| `ftspawn` | player | one draugr, feeble on purpose: keeps a follower in combat without threatening them |
| `ftbear` | player | one cave bear (`00023A8B`) — hits hard enough to cross a threshold through real damage |
| `ftman` | player | one level-1 Nord bandit (`0003DE8A`) — a human enemy, so Voice of the Emperor's calm has someone to land on; a bear shows nothing |
| `fthurt` | selection | `damageav health 100`. One command, deliberately: a before/after print in the same file cannot be trusted to bracket the damage |
| `ftstatus` | selection | health/magicka/stamina, `isincombat`, combat target, and both potion tiers |
| `ftclean` | selection | restore and un-follow. **Non-destructive** — safe on a real quest NPC |

Verification lives in `ftstatus`, run as its own command, precisely because a check inside
the file it is checking may execute before the thing it checks.

**Potion FormIDs**, all read off the running game — weak / strong:

| | Weak (×10) | Strong (×5) |
|---|---|---|
| Healing | `0003EADD` | `0003EADE` |
| Magicka | `0003EAE0` | `0003EAE1` |
| Stamina | `0003EAE5` | `00039BE8` |

Two tiers on purpose: the engine drinks the **strongest** it carries, so watching which count
falls proves it chose correctly rather than merely drinking something. Note stamina breaks
the adjacent-pair pattern the other two follow — `00039BE8` is nowhere near `0003EAE5`, and
inferring it as `0003EAE6` was wrong. A pattern that holds twice is not a rule.

### Why these three, and why real references

Real followers beat spawned copies: actual levelled stats, spells and gear; no duplicate
NPCs accumulating in the save; and `prid` selects them, so no manual click step.

They also cover three resource profiles, which matters because a rule can only be tested
against a follower who has the resource in question:

| | Class | Useful for |
|---|---|---|
| Marcurio | Destruction Mage | magicka rules — a real pool that actually moves |
| Jenassa | Ranger | stamina, and ranged behaviour |
| Lydia | Warrior | *retired from this harness* — her magicka sits at 50/50 forever |

Three followers also exercises the multi-follower paths for the first time: three menu
entries, three slots, three independent evaluation contexts with separate cooldowns.

**Never run deletion commands on these.** They are real quest NPCs; `markfordelete` would
remove Marcurio from The Bee and Barb permanently. `ftclean` is deliberately non-destructive
for this reason.

## About the test cell

`coc QASmoke` is Bethesda's **Editor Smoke Test Cell**. It is the well-known dev room and
it is a reasonable choice, with one feature our earlier notes missed: besides the item
chests, it has an **arena section with buttons that spawn sets of mutually hostile
enemies**. That is a considerably better fight than the single draugr `ftspawn` gives you,
and worth walking over to once the potion rule works.

Two caveats, both unverified on this install and worth checking before relying on them:

- **Navmesh quality is the thing that matters** for this project, more than lighting or
  loot. A follower that cannot path is a follower whose tactics cannot be evaluated, and a
  poorly navmeshed test cell would produce failures that look like bugs in our rules engine
  but are not. If Lydia gets stuck on scenery in QASmoke, move the scenario elsewhere before
  concluding anything about the mod.
- **`coc` inside a batch file** reportedly either fails silently or hangs. Community
  reports, not something measured here — but the cost of testing it is a hung game, so
  `ftall` keeps the travel step manual.

If QASmoke turns out to be a poor arena,
[Spawn Arena Modders Resource](https://www.nexusmods.com/skyrimspecialedition/mods/168219)
adds a purpose-built one (`coc EEarena`) with spawn markers and group-spawn activators.

## Later: scripted instead of typed

[ConsoleUtilSSE](https://www.nexusmods.com/skyrimspecialedition/mods/24858) (or the
[Extended fork](https://github.com/KrisV-777/ConsoleUtil-Extended)) exposes:

```papyrus
scriptname ConsoleUtil Hidden
int function GetVersion() global native
function PrintMessage(string text) global native
function ExecuteCommand(string text) global native
ObjectReference function GetSelectedReference() global native
function SetSelectedReference(ObjectReference obj) global native
```

That turns the whole scenario into a script — useful once the rules engine exists and you
want to run the same fight twenty times with different rule sets. Check its Files tab for a
build matching 1.6.1170; the last version info I found referenced 1.6.640.

## FormIDs used — confidence

Verify these on first run rather than trusting them. Cross-checked where noted.

| Thing | FormID | Confidence |
|---|---|---|
| Lydia (RefID, placed instance) | `000A2C94` | High — UESP + Fandom agree |
| Lydia (BaseID) | `000A2C8E` | High — same two sources |
| Draugr, weakest generic | `000387C0` | High — two Fandom pages agree |
| Skeever | `000EF610` | High — two sources |
| Mudcrab | `00000EB2` | High — two sources |
| Potion of Minor Healing | `0003EADD` | High — two sources |
| Potion of Healing | `0003EADE` | Medium — single source |
| Bandit, generic | `001068FE` | **Medium — single source, verify** |
| CurrentFollowerFaction | `0005C84E` | High — two sources |

## First run is a verification pass

I wrote these from documentation, not from a running game. Treat run #1 as a checklist:

- [ ] `bat ftsetup` found the file (root vs `Data\`)
- [ ] Lydia appears next to you and follows
- [ ] `getavinfo health` prints a max value — record it, edit `fthurt`
- [ ] She has the potions in her inventory
- [ ] `bat ftspawn` produces a draugr that attacks
- [ ] `bat fthurt` drops her health and the drop is visible
- [ ] **Does she drink a potion on her own?** Vanilla NPCs handle player-given potions
      badly — that's the whole premise of NPCsUsePotions. If she never drinks unprompted,
      that's the baseline our Phase 1 rule has to beat, and worth noting.
- [ ] `bat ftclean` restores her

## FormIDs: base, not reference

Every actor in this harness is spawned with `player.placeatme <BaseID>`, never selected with
`prid <RefID>`. The distinction has bitten once already and is worth stating plainly:

| Follower | BaseID (use this) | RefID (do not) | Where their reference lives |
|---|---|---|---|
| Marcurio | `000B9980` | `000B9986` | The Bee and Barb, Riften |
| Jenassa | `000B9982` | `000E1BA9` | The Drunken Huntsman, Whiterun |
| Lydia | `000A2C8E` | `000A2C94` | Dragonsreach — **disabled outright** until you are Thane |

A **RefID names an existing instance**, and that instance is not reachable until the game
has initialised it — which for both of these means visiting a city and, for Lydia,
completing a quest. On a fresh character `prid` selects nothing and every command after it
silently does nothing, which looks exactly like a broken batch file.

A **BaseID names the template**, which exists from the moment the game starts. `placeatme`
takes the base form, so it works at any point in any save, in any cell, regardless of quest
state — and the copy is disposable, which is what you want in a test anyway.

`player.placeatme 000B9986 1` fails for this reason: it asks the game to instantiate a
specific existing instance, which is the very thing that does not exist yet.

### Loading the cell initialises the reference — verified

`coc` into the follower's home cell once, then `coc` back, and `prid <RefID>` works from
then on. Confirmed in game for both Marcurio and Jenassa. They are persistent references,
so once instantiated they stay reachable.

This works for **hirelings**, who simply stand in a bar. It does **not** work for Lydia: she
is `disable`d outright until the Thane quest completes, so loading Dragonsreach instantiates
nothing. That is the difference between a reference that has not been *created* and one that
has been created and switched *off*, and it is why the BaseID route existed at all.
