# FollowerTactics — project context

A Dragon Age: Origins-style tactics system for Skyrim SE/AE followers: an ordered list of
`IF <subject>: <condition> THEN <target>: <action>` rules, per follower, editable in game.

Read `docs/PLAN.md` first. `docs/RESEARCH.md` has the sourced findings behind it, with
explicit uncertainty flags. `docs/MAGIC.md` is how casting works and what does not;
`docs/PROFILES.md` is how tactics live in the save (format, when, versioning);
`docs/UNIQUE.md` is how one copy of an item is told from another (the engine's
unique id, what a pin, a ban and a rule name, what is still to verify);
`docs/GAME_MODEL.md` is the plan for a model of the engine the item logic can be
tested against without Skyrim (not yet built; what is known, what to measure first);
`docs/DEVBENCH.md` is what devbench, a plugin that lets a script or an agent drive a running Skyrim, would give us, its limits, and how it meets the game model (observations; nothing wired in);
`docs/LOGGING.md` is the log design (levels, structured events, the JSON-lines sidecar);
`docs/EVENTS.md` is the game events: what tactics record, the last 1,000 kept in memory for the panel, and the per-session files and their archive (designed; not yet built);
`docs/MODIFIERS.md` is where a follower's bonuses come from and how to total them (perk
entry points over actor values; research and thoughts, with the open questions);
`docs/TODO.md` is what is still to do.

## The one architectural rule

**No `RE::` type may cross into `src/core/`.** There is no headless test harness for
Skyrim, so the only route to automated tests is logic that does not need the game.
`src/core/` (Snapshot, Rule, Evaluator) compiles with no Skyrim, no SKSE, no CommonLibSSE
and is covered by Catch2. `src/game/` holds every `RE::` call and is thin, imperative, and
verified by playing. If you want to `#include "RE/Skyrim.h"` in `core/`, the code belongs in
`game/`.

## Environment — verified, do not "fix"

| | |
|---|---|
| Skyrim runtime | **1.6.1170** (deliberately downgraded from 1.7.104) |
| Creation Kit | **1.6.1378.1** (downgraded to match; CKPE supports it, 1.7.99 is unsupported) |
| SKSE | 2.2.8, `skse64_1_6_1170.dll` |
| Address Library | AIO v13, installed at `Data/SKSE/Plugins/` |
| Game path | `C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition` |
| MO2 instance | `C:\modding\mo2` (ModOrganizer.ini lives there); its mods folder is `MO2\mods` inside this repo (gitignored). houseCARL is pointed at it. |

`python tools/check_install.py "<game folder>"` verifies all of the above and detects drift.

**Never run Steam's "verify integrity of game files"** — it restores 1.7.104 and undoes the
downgrade. Both appmanifests are read-only to prevent silent updates. **Never launch Skyrim
or the CK from the Steam Library** — use `skse64_loader.exe` or MO2. See `docs/DOWNGRADE.md`.

## Building

Use `tools\build.ps1` from any shell. It locates the VS install, imports `vcvars64.bat`
into the session, and then runs CMake -- so the "x64 Native Tools Command Prompt" is no
longer a prerequisite. (`CMakePresets.json` still pins `CMAKE_CXX_COMPILER=cl.exe`, so a
bare `cmake --preset` outside that environment still fails; the script is the supported
entry point.)

```powershell
.\tools\build.ps1 -Preset core  -Test      # rule engine + tests. Seconds. Use constantly.
.\tools\build.ps1 -Preset debug            # SKSE plugin. First run builds CommonLibSSE-NG.
.\tools\build.ps1 -Preset core-asan -Test  # same core tests, under AddressSanitizer
.\tools\build.ps1 -Preset core-cov -Coverage  # same, under clang-cl; which lines the tests reach
.\tools\build.ps1 -Preset debug -Analyze -NoDeploy  # any preset, under MSVC's static analyser
```

`-Fresh` wipes the preset's build directory first. `-NoDeploy` builds the plugin
without copying it into the mods folder and without the running-game guard, for
compiling while Skyrim is up (it holds the deployed DLL open); the next plain
run copies as usual. `-Analyze` compiles our targets with `/analyze` (findings are
C6xxx warnings; several times slower; the cached flag recompiles our sources on the
way in and out). `-Coverage` needs `core-cov`, runs the tests once and prints per-file
line coverage of `src/core`, with the line-by-line HTML in `build\core-cov\coverage\html`.

`core` / `core-asan` / `core-cov` need no vcpkg and no Skyrim at all -- that is the fast
feedback loop. `debug` / `release` build the plugin and pull CommonLibSSE-NG through vcpkg.
Every tool here ships inside the VS install: `clang-cl`, `llvm-cov`, `clang-tidy` and the
sanitizer runtimes are the "C++ Clang tools for Windows" component. ASan on MSVC finds
memory misuse, not leaks (LeakSanitizer has no Windows build); UBSan is clang-only and
its runtime is present, unused so far.

`.\tools\package.ps1` builds the release plugin and writes `dist\follower-tactics-<version>.zip`, a mod root (one DLL and a README) to install from the archive in Mod Organizer; the version is `project(... VERSION)` in CMakeLists.txt. `dist/` is ignored.

`SKYRIM_MODS_FOLDER` is set to `MO2\mods`; the build deploys to
`MO2\mods\FollowerTactics\SKSE\Plugins\`. New mods appear **unticked** in MO2 -- tick it
or the DLL never loads.

Last verified green under MSVC 19.42 (`core`, `core-asan`) and clang-cl 18 (`core-cov`), 2026-09-09. Counts -- how many cases, what percentage covered -- are deliberately not kept here: they move with every test added, and a number that goes stale in a week teaches you to distrust the page. Run the presets and read the numbers off them.

## After every edit

Run these. They are fast, and each one has already caught something real in this
project:

```powershell
.\tools\build.ps1 -Preset core -Test        # 1. tests
cmake --build --preset core --target format # 2. formatter, rewrites in place
cmake --build --preset core --target tidy   # 3. linter, src/core (seconds; only what changed)
.\tools\build.ps1 -Preset debug             # 4. plugin builds and deploys
```

(2) and (3) need the developer environment, so run them from a shell where
`tools\build.ps1` has already imported it, or wrap them the same way it does.
(3) reads `.clang-tidy` at the repo root: the bugprone, performance, analyzer,
concurrency and misc groups, nothing stylistic; the file says what is excluded and
why, and names the one known false positive. It runs one clang-tidy per file, so
Ninja spreads them across cores and skips the files that have not changed: a full
pass is ~99 s, one touched file ~6.6 s, nothing changed ~3.6 s. Delete
`build\<preset>\tidy` to force a full pass. `cmake/ClangTools.cmake` has the
measurements behind that: the per-file cost is the checks walking CommonLibSSE's
inlined header bodies, and no filter avoids it.

Before anything is called done, all four must be green, plus both of these:

```powershell
.\tools\build.ps1 -Preset core-asan -Test   # AddressSanitizer
cmake --build --preset debug --target tidy  # the linter over src/game too
```

The second is the slower one -- every `src/game` translation unit parses the whole of CommonLibSSE, which no filter avoids and which the `/Y-` above means clang cannot precompile once and reuse. Spread across cores it is about a minute and a half against `src/core`'s twenty seconds, which is why it sits here rather than in the fast loop.

**A green build is not a passing check.** Every one of these has caught a defect
that compiled perfectly: the tests caught a cooldown interaction that changed
behaviour silently, the formatter has caught hand-written code on nearly every
pass, and extending the linter to `src/game` found dead code within a minute.

**Do not report work as finished without running them.** "It compiles" is the
weakest signal available here -- the whole point of the `RE::`-free core is that
there IS a real check, so use it.

### The linter's blind spot, and how it hid

**Each preset's `tidy` lints what its own compile database covers, and no more**: the core presets lint `src/core`, and `src/game` -- which appears only in the *plugin's* database -- is covered by `cmake --build --preset debug --target tidy`. Getting `src/game` covered needs two
flags that are easy to get wrong:

- `--header-filter=src.(core|game)` keeps CommonLibSSE's thousands of header
  lines quiet while still checking ours.
- `--extra-arg-before=/Y-` disables the precompiled header. MSVC's `.pch` is not
  a format clang can read, and **without this clang-tidy fails outright** with
  `not a valid precompiled PCH file` -- while reporting zero findings, which
  looks exactly like a clean run. If tidy ever reports nothing on a file you
  know is messy, check it actually parsed.

Until 2026-09-09 `tidy` reached across presets instead, pointing `-p` at `build/debug` whenever that directory existed. That test is answered at CONFIGURE time, so it went stale in exactly the tree that most needs it -- a fresh clone, or a new worktree -- and clang-tidy then parsed `src/game` with guessed flags and buried the real findings under `no type named 'string_view' in namespace 'std'` and `inline variables are a C++17 extension` -- errors that look like a catastrophe in our own headers and mean nothing at all. It is the `.pch` blind spot wearing the opposite mask, a catastrophic-looking run rather than a clean-looking one, and it is answered the same way: **check that it actually parsed.**

## Toolchain gotchas already hit

- **Visual Studio can be installed without the C++ workload.** That was this machine's
  actual state: VS 2022 Community 17.12.4 present, but no `VC\Tools\MSVC`, no
  `vcvars64.bat`, no CMake, no Ninja. It presents as "cl.exe not found", which reads like
  a PATH problem and is not.
- **The VS installer refuses to modify an in-use install** -- exit code **8006**,
  `Pre-check verification failed with warning(s): VSProcessesRunning`. Close Visual Studio
  first. That failure is only visible in `%TEMP%\dd_installer_*.log`; the launching
  command itself reports success.
- **`--installPath` must be passed as one already-quoted token.** The installer's own
  parser splits on spaces otherwise, reports `installPath: C:\Program`, and exits 1.
- **`setup.exe` has no `--wait`** (that is `vs_installer.exe`); passing it exits 87 having
  done nothing at all.
- **`vswhere -latest -requires ...` finds nothing after a `--norestart` install.** The
  instance is flagged `isComplete: False` and reports an empty package list even though
  `cl.exe` works fine. `tools\build.ps1` probes for `vcvars64.bat` on disk instead.
- **`vcvars64.bat` overwrites `VCPKG_ROOT`** with the vcpkg bundled inside Visual Studio.
  That is not our vcpkg, so the build resolves the dependencies against the wrong
  package tree and fails somewhere unrelated-looking
  (a `fmt` build failure, in our case). `tools\build.ps1` saves and restores it.
- **Do not put `-DUNICODE`/`-D_UNICODE` in the global `CMAKE_CXX_FLAGS`.** They are the
  plugin's concern and are set on that target only. Globally they leak into third-party
  code built in the same tree: Catch2's `catch_main.cpp` compiles `wmain` instead of
  `main` under `_UNICODE`, and the test executable then fails to link with
  `unresolved external symbol main` -- an error pointing nowhere near the cause.

## SKSE gotchas already hit

- **A task must never re-arm itself via `AddTask`.** This hangs the game on the first
  frame, with no crash log and no error -- the process simply stops responding.

  SKSE drains its task queue to empty inside one call
  (`skse64/Hooks_Threads.cpp`):

  ```cpp
  void BSTaskPool::ProcessTasks() {
      CALL_MEMBER_FN(this, ProcessTaskQueue_HookTarget)();
      while (!IsTaskQueueEmpty()) { cmd->Run(); cmd->Dispose(); }
  }
  ```

  A task that calls `AddTask` from inside its own `Run()` refills the queue faster than
  the loop drains it, so `ProcessTasks` never returns and the main thread spins forever.
  **`AddTask` is a "do this once, on the game thread" primitive, not a scheduler.**

  For periodic work: pace on a separate thread and have it `AddTask` once per interval.
  The task does the game-thread work and returns. See `src/game/Tactics.cpp`.

  SKSE ships its own source in `SKSE/src/`, which is how this was diagnosed rather than
  guessed at. Worth remembering it is there.

- **An NPC's equipment changes are queued to her next update, and the frozen
  clock withholds it.** `ActorEquipManager::EquipObject` with the queue flag
  set (the potion path's shape) shows nothing while the panel has time
  frozen; the click path clears that flag and calls `Actor::Update3DModel`,
  and the item and model change at once. The item's ENCHANTMENT still waits
  for her next update. Do not apply it early with `UpdateArmorAbility`: the
  engine applies it again when time runs, and the effect doubles.
- **Prevent-removal (the force flag on EquipObject) is worn-item state in
  the save, and it outlives the mod.** Verified in play (2026-09-04): with the
  DLL removed, the engine's equip-best swap had its unequip of a pinned dagger
  refused by the flag while its equip of the new sword went ahead, leaving
  both marked equipped in one hand. Pins no longer set it; the equip detour,
  the score hook and the watchdog in `Pins.cpp` keep the pin instead, and all
  three go away with the DLL. Also verified earlier: neither the flag nor the
  detour holds against a SPELL equip, the combat AI's own or our UseMagic
  package's; the watchdog puts the item back, by the core's `PutBackNow`: at
  once, except while one of our casts holds the hand. Tested against a
  simulation in `tests/test_loadout.cpp`.

## CommonLibSSE gotchas already hit

Verified against the headers. Do not "simplify" these away:

- **`logger` is not defined by CommonLibSSE-NG.** `namespace logger = SKSE::log;` in `PCH.h`
  is ours. Tutorials imply the library provides it. It does not.
- **`spdlog` sinks are not in NG's PCH.** `<spdlog/sinks/basic_file_sink.h>` must be
  included explicitly.
- **`SKSE::Trampoline::write_branch` / `write_call` are not function detours.** They
  overwrite an EXISTING jump or call instruction and preserve nothing, so pointing one at
  a function's first bytes corrupts its prologue. A function-entry hook (the
  `ActorEquipManager::EquipObject` detour in `Pins.cpp`) goes through Microsoft Detours
  (vcpkg `detours`), which relocates the displaced instructions and returns a callable
  original. The score hook is a vtable write and needs neither.
- **`SKSE::PluginDeclaration::GetSingleton()` does not exist.** The CMake generates a global
  `SKSEPlugin_Version` via `SKSEPluginInfo(...)`. Use a literal name instead.
- **`SKSE::Init` sets up logging unless told not to.** Its default `InitInfo{.log = true}` opens
  `<plugin>.log` with truncation, installs its own logger as spdlog's default at info (debug in a
  debug build) and writes a version banner. Called after our own `log::Init`, it silently replaced
  ours: every line went through its logger, the ini's `level` never applied to a release build,
  and our banner lines were truncated away. Found 2026-09-11, after two days of "debug does
  nothing" in Nordic Souls. We call `SKSE::Init(skse, {.log = false})`.
- **Never call `ActiveEffect::GetTargetActor()` or `MagicTarget::GetTargetAsActor()`.** They return a pointer 0x98/0xA0 into the actor, and it crashed the game (2026-09-13). Use `target->GetTargetStatsObject()` and `As<RE::Actor>()`. `docs/COMMONLIB.md` has the evidence and history.

We are on **alandtse/CommonLibSSE-NG v7.5.1** as the submodule `extern/commonlibsse-ng`
(clone with `--recurse-submodules`). Its vcpkg dependencies are in our manifest.
`docs/COMMONLIB.md` has why the fork was chosen and the API shapes worth knowing.

## Current phase

**Phase 0 — COMPLETE** (2026-09-01 16:55). Build, deploy, load under SKSE at 1.6.1170.

**Phase 1 — COMPLETE** (2026-09-01 19:07). The go/no-go risk is retired: a follower
reliably drinks a health potion when her health crosses 50%, driven by the rule engine.
Measured in-game, from `FollowerTactics.log`:

```
Lydia (FF000DE0) health 25/42 (59%) combat=true potions=13     <- above threshold, no fire
Lydia (FF000DE0) FIRED rule 0 "emergency heal" -> performed [health 21/42 = 49%]
Lydia (FF000DE0) health 42/42 (100%) combat=true potions=12    <- drank, count dropped
tactics: 23 evaluations, avg 47 us, max 50 us
```

`docs/PLAN.md` section 5 rated "forcing an NPC to drink a potion isn't reliable" as the
highest risk in the project, the one that would kill the marquee feature. It works, via
`ActorEquipManager::EquipObject` with NPCsUsePotions' parameters.

### Performance: measured, and no optimisation needed

**47 us average, 68 us worst case** per follower evaluation, including the inventory scan.
At the original 150 ms tick that was ~0.3 ms/second for one follower; eight followers
~2.6 ms/second, about **0.04 ms/frame amortised at 60 fps** against a 0.5 ms/frame budget.
The tick is now 500 ms (one decision per half-second "turn"), so the cost is lower still.

So the three things `PLAN.md` 3.2/3.3 called for -- staggered scheduling, cached expensive
sensors, dependency-driven sensor activation -- are **not needed yet**, and building them
now would be optimising a cost that is two orders of magnitude under budget. Revisit only
if this number moves. It is logged every 5 s of combat, so drift is visible.

**Casting — WORKS end to end (2026-09-02 13:15).** `docs/MAGIC.md` "The eighth attempt":
UseMagic packages put at the front of the follower's own package stack, gated by a
condition held by a lease (a faction rank until 2026-09-08), released when the follower's own spell-fire animation event
names our spell. Measured over two cycles: rule fires at 43% health, package selected on
the same tick, `Fast Healing -- OURS` 1.4 s later, health 75 -> 175, released next tick,
follower back to fighting. Since 2026-09-08 the records are **made in memory** (`src/game/Forms.cpp`, `docs/MAGIC.md` "Forms at runtime"): no plugin
file, nothing of ours in the save, the DLL is the whole mod. Verified in play
the same day; the ESP is gone (git history before 2026-09-08 has it). Since 2026-09-14 they are one set per follower, made when the tick first sees them, instead of a shared pool of sixteen made at load; not yet verified in play.
Recruit through dialogue (or `cqf DialogueFollower
SetFollower`), never `setplayerteammate`. Cooldowns and leases run on game time.

**Phase 2 — in progress.** The rule engine has the subject/predicate model, a list of
actions per rule done one per tick, equip actions that pin, and conditions for status,
armour, resistance, attacked-by, the party's extremes and the player's fight
(`docs/CONDITIONS.md`); `Snapshot::allies` / `enemies` are populated by definition
(the party, and whoever the compass paints red). Rules persist in the SKSE co-save:
one JSON record per follower with the rules, the switch and the player's pins, written
when the game saves and taken back when the tick first sees the follower after a load
(`docs/PROFILES.md`; built 2026-09-04, not yet verified in play). Still missing:
shareable named profiles. Actions to come are in `docs/ACTIONS.md`.

**Defaults (2026-09-04):** a follower starts with NO rules; both switches start on,
which is safe because an empty list does nothing. A fresh install changes nothing
until a rule is written. The Phase 1 "emergency heal" rule above was the hardcoded
default until then.

## Where the log is

```
%USERPROFILE%\OneDrive\Documents\My Games\Skyrim Special Edition\SKSE\FollowerTactics.log
%USERPROFILE%\OneDrive\Documents\My Games\Skyrim Special Edition\SKSE\FollowerTactics.events.jsonl
```

Documents is redirected to OneDrive on this machine. Resolve it with
`[Environment]::GetFolderPath('MyDocuments')`; never assume `%USERPROFILE%\Documents`.
SKSE's own `skse64.log` is in the same folder.

The first is prose to read while playing: every line, filtered by the level set in `Data/SKSE/Plugins/FollowerTactics.ini` — `info` by default, `debug` for the per-tick readouts. The second is the game events, what tactics did to and saw of a follower, one JSON object per line, whatever the level; a game event is one call that writes both. `docs/LOGGING.md` is the machinery, the levels and how to add a call site (`ft::log::<module>.info(...)`; there is no `logger` alias any more); `docs/EVENTS.md` is which events there are.

## Reading the executable

`tools/disasm.py <address-library-id>` disassembles a function from
SkyrimSE.exe; `--vtable <id>` dumps a vtable, `--lookup <rva>` names the
function an address falls in. The installed exe is SteamStub-encrypted and
reads as noise: point `SKYRIM_EXE` at a copy unpacked with Steamless (never
the installed file). While the game is running, `tools/livedisasm.py` reads
the decrypted code out of the live process instead, with the same modes plus
`--callers <id>` (every call into a function) and `--bytes`. `docs/MAGIC.md`
"Forms at runtime" and `docs/UNIQUE.md` are what has been read with them so
far.

## Fetching UESP / Nexus pages

`WebFetch` is blocked on `uesp.net` (both subdomains) and `nexusmods.com`. Don't retry it or spoof curl headers at these -- use, respectively, the `search-uesp` skill, the `search-creation-kit-wiki` skill, and houseCARL's keyless Nexus tools (`housecarl_nexus_*`, see its MCP instructions) for the verified working methods.

## Working style

State uncertainty explicitly rather than asserting. Verify against headers or the running
system rather than from memory — several of the notes above exist because an assumption was
wrong. Prefer measuring to estimating.
