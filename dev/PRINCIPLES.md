# Principles

How code that meets the engine is written here: the rules this project learned, each with the incident that taught it and where it is applied. The detail lives in the documents each one names; this is the list to read before writing a feature that touches the game.

## Where the logic lives

**Logic in core, the engine in game.** A decision made from facts already read goes into `src/core`, over a plain description of those facts, where Catch2 tests it; `src/game` reads the engine, hands the facts over and performs what comes back. This is the one architectural rule (`CLAUDE.md`), and `dev/TESTING.md` is the ledger of what has moved. There is no headless Skyrim, so a decision left in the game is verified only by playing.

**State machines for anything that spans frames.** A bash, a power attack, a cast: steps, each with its window, and one decision per step from everything read at once. The shape that worked every time is the same: the machine's state and a struct of what was read go in, a decision comes out, and the engine actions go through a callback the test scripts (`AdvanceBash`, `AdvanceStrike`, `AdvancePlayerCast`, `AdvanceCast`). The machine never touches the engine, which is what lets the first rule hold.

**One owner per fact, and an approach removed the day it is abandoned.** Three animation listeners each kept their own flags, lock and rules for clearing them, until one did it for all (`core/GraphEvents.h`). What the player could do was a hand-kept list beside the switch that did it, until one table said both (`core/Routes.h`). The UseWeapon power attack's code outlived four runs in play and its replacement, spread through the cast slots as a third kind of slot. A diff far larger than its concept means one idea was smeared across parallel state: fix the shape before committing.

## Meeting the engine

**Events over polls and timing.** Wait for the event that says a thing is done, and act on it from the listener rather than a flag read later. A step keyed to an event cannot race what it waits for. A 0.25 s settle before a bash lost a request to the follower's AI swinging again 116 ms after the block was ready, and the block's own `blockStartOut` replaced it (`dev/ATTACK.md`); a press made before the equip's `InterruptCast` was cut short by it, and a press on that `InterruptCast`, the equip's start rather than its end, was cut short by a second equip's with a staff in the other hand (`dev/PLAYER.md`). Where the engine sends nothing -- a caster reaching Ready -- read the state, never guess a time (`dev/PLAYER.md`, "Why the cast keeps a 50 ms tick").

**The engine's own path.** Call what the engine calls rather than simulate it, and whatever it does on the way comes along. A potion is drunk through the equip call the game consumes items through (`game/Actions.cpp`). A power attack is the combat AI's own attack action: the UseWeapon package drew its attack without looking at the hands, and bare animation events landed 3 of 11 (`dev/ATTACK.md`). The player's cast is a press of their own controls (`dev/PLAYER.md`). The design form of this is `dev/PLAN.md` section 1: bias the native AI, do not puppet the actor.

**Read the engine; do not trust the wiki or memory.** The executable and the headers are the authority (`tools/disasm.py`, Ghidra, `CLAUDE.md` "Reading the executable"). UESP's stamina formula priced a power attack at half (`dev/ACTIONS.md`); CommonLib declares `UseSkill` one argument short (`dev/COMMONLIB.md`); a package's Self target was taken to be 5 and the engine reads 6 (`game/Packages.cpp`). Every address is an (SE, AE) pair named in `src/game/Addresses.h` and read on both builds (`dev/VERSIONS.md`).

**Know which thread you are on.** Engine events come on their own threads: note them there, act on the game thread. The graph's sink records and queues a step; the step runs on the game thread (`game/Graph.cpp`). SKSE's task queue runs a task once, and a task that queues itself hangs the game (`CLAUDE.md`, "SKSE gotchas").

**No engine pointer across a tick.** Keep ids and handles, and look them up again. A worn item's extra list kept across a cast was freed by the engine and handed back to `EquipObject` dangling (crash 2026-09-18, `game/PlayerCast.cpp`). `dev/TESTING.md` "Identity across time" is the contract for every handle that crosses a tick.

## Holding and letting go

**Every wait has a deadline and ends for one named reason.** Nothing is held past its deadline whatever the game does or does not do, and the reason says what it waited on: "deadline, target never in front" says what to fix, where a request that silently hangs says nothing. Each request reports through `rule.resolved` (`dev/EVENTS.md`).

**Borrow and give back, on every path, and leave no trace.** What is changed on the engine's side is restored when the request ends, a load included: the hands and the voice lent to the player's cast, a shout's trimmed words, a power's spell type, a package's target. What outlives the DLL is the danger: the prevent-removal flag stayed in the save after the mod was gone and left two items equipped in one hand (`CLAUDE.md`, "SKSE gotchas"), and the records are made in memory, with no plugin file in the save (`dev/MAGIC.md`, "Forms at runtime").

## Knowing it works

**Log what each decision read, not only what it decided.** A reason at info for every outcome, and each step's reads at debug (`ReadsOf`), so that a session in play is a test fixture: `tests/test_replay.cpp` replays sequences from the log through the same watches and machines (`dev/LOGGING.md`, `dev/TESTING.md`). A log that says only "failed" means reproducing the bug by hand.

**Check that the tests would catch a bug.** A green suite can pass while testing nothing. Coverage (`core-cov -Coverage`) shows what never runs; breaking the code on purpose shows what is never checked: fourteen breaks of the graph listener each failed the suite, and two stopped failing when a test came out, until tests of their own went in (2026-09-25).

**Random sequences, with something that knows the answer.** Hand-written tests cover the orders someone imagined; the engine delivers events in any order its threads do. Random streams need an oracle: another implementation, as the new graph listener was run beside the three it replaced; a property that must always hold (a request ends by its deadline, a lent hand is given back); or, for a parser of what is on disk -- the co-save, a profile -- that nothing crashes, which is fuzzing's ground. Fixed seeds, so a failure reproduces; generators weighted toward the states that matter; counts modest enough to run with every build.
