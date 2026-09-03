# FollowerTactics — project context

A Dragon Age: Origins-style tactics system for Skyrim SE/AE followers: an ordered list of
`IF <condition> THEN <action> ON <target>` rules, per follower, editable in game.

Read `docs/PLAN.md` first. `docs/RESEARCH.md` has the sourced findings behind it, with
explicit uncertainty flags. `docs/MAGIC.md` is how casting works and what does not;
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
```

`-Fresh` wipes the preset's build directory first.

`core` / `core-asan` need no vcpkg and no Skyrim at all -- that is the fast feedback loop.
`debug` / `release` build the plugin and pull CommonLibSSE-NG through vcpkg.

`SKYRIM_MODS_FOLDER` is set to `MO2\mods`; the build deploys to
`MO2\mods\FollowerTactics\SKSE\Plugins\`. New mods appear **unticked** in MO2 -- tick it
or the DLL never loads.

Last verified: 10 cases / 32 assertions green under MSVC 19.42 (`core` preset).

## After every edit

Run these. They are fast, and each one has already caught something real in this
project:

```powershell
.\tools\build.ps1 -Preset core -Test        # 1. tests
cmake --build --preset core --target format # 2. formatter, rewrites in place
cmake --build --preset core --target tidy   # 3. linter
.\tools\build.ps1 -Preset debug             # 4. plugin builds and deploys
```

(2) and (3) need the developer environment, so run them from a shell where
`tools\build.ps1` has already imported it, or wrap them the same way it does.

Before anything is called done, all four must be green, plus:

```powershell
.\tools\build.ps1 -Preset core-asan -Test   # AddressSanitizer, before committing
```

**A green build is not a passing check.** Every one of these has caught a defect
that compiled perfectly: the tests caught a cooldown interaction that changed
behaviour silently, the formatter has caught hand-written code on nearly every
pass, and extending the linter to `src/game` found dead code within a minute.

**Do not report work as finished without running them.** "It compiles" is the
weakest signal available here -- the whole point of the `RE::`-free core is that
there IS a real check, so use it.

### The linter's blind spot, and how it hid

`tidy` covers `src/core` **and** `src/game`. Getting `src/game` covered needs two
flags that are easy to get wrong:

- `--header-filter=src.(core|game)` keeps CommonLibSSE's thousands of header
  lines quiet while still checking ours.
- `--extra-arg-before=/Y-` disables the precompiled header. MSVC's `.pch` is not
  a format clang can read, and **without this clang-tidy fails outright** with
  `not a valid precompiled PCH file` -- while reporting zero findings, which
  looks exactly like a clean run. If tidy ever reports nothing on a file you
  know is messy, check it actually parsed.

`src/game` also only appears in the *plugin's* compile database, which the
core-only presets never generate, so `tidy` points at `build/debug` when that
exists.

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
  That is not our vcpkg and does not have the colorglass registry, so the build resolves
  CommonLibSSE-NG against the wrong package tree and fails somewhere unrelated-looking
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
- **Prevent-removal (the force flag on EquipObject) holds against the
  equip-best swap but not against the combat AI's spell hand.** Verified in
  play: pinned robes stop iron armour going on; a pinned dagger comes off the
  moment a mage wants that hand for a spell. See the pin watchdog in
  `Tactics.cpp`.

## CommonLibSSE gotchas already hit

Verified against the 3.7.0 headers. Do not "simplify" these away:

- **`logger` is not defined by CommonLibSSE-NG.** `namespace logger = SKSE::log;` in `PCH.h`
  is ours. Tutorials imply the library provides it. It does not.
- **`spdlog` sinks are not in NG's PCH.** `<spdlog/sinks/basic_file_sink.h>` must be
  included explicitly.
- **`SKSE::PluginDeclaration::GetSingleton()` does not exist.** The CMake generates a global
  `SKSEPlugin_Version` via `SKSEPluginInfo(...)`. Use a literal name instead.

We are on **CharmedBaryon** CommonLibSSE-NG 3.7.0 via the colorglass vcpkg registry
(baseline bumped to registry HEAD; the template's pin served 3.6.0, which predates our
runtime). That fork is stale since Sept 2024 but postdates 1.6.1170, and plugins declare
`VersionIndependence::AddressLibrary` so offsets resolve at load. The live fork is
alandtse/CommonLibSSE-NG v7.0.0 — see `docs/COMMONLIB.md` for when and how to migrate.

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
UseMagic packages spliced into the follower alias's combat-override list, gated by a
faction rank held by a lease, released when the follower's own spell-fire animation event
names our spell. Measured over two cycles: rule fires at 43% health, package selected on
the same tick, `Fast Healing -- OURS` 1.4 s later, health 75 -> 175, released next tick,
follower back to fighting. The ESL is versioned at `esp/FollowerTactics.esp` (edit with
houseCARL, not the xEdit script). Recruit through dialogue (or `cqf DialogueFollower
SetFollower`), never `setplayerteammate`. Cooldowns and leases run on game time.

**Phase 2 — next.** The rule engine already has the subject/predicate model and is unit
tested; what is missing is JSON load/save (the shareable profile format), per-follower
profiles, and populating `Snapshot::enemies` / `allies` so group subjects work at all.

## Where the log actually is — not where you would guess

```
%USERPROFILE%\OneDrive\Documents\My Games\Skyrim.INI\SKSE\FollowerTactics.log
```

Two independent surprises stack up in that path, both verified on this machine:

1. **Documents is redirected to OneDrive.** Resolve it with
   `[Environment]::GetFolderPath('MyDocuments')`; never assume `%USERPROFILE%\Documents`.
2. **The subfolder is `Skyrim.INI`, not `Skyrim Special Edition`.** This is a
   CommonLibSSE-NG bug, not a misconfiguration. `SKSE::log::log_directory()` does not
   hardcode the name — it reads a relocated global out of the game binary:

   ```cpp
   path /= *REL::Relocation<const char**>(RELOCATION_ID(508778, 380738)).get();
   ```

   On **1.6.1170** with CharmedBaryon 3.7.0 that address resolves to a string holding
   `"Skyrim.INI"`. SKSE64 itself gets it right and writes `skse64.log` to
   `My Games\Skyrim Special Edition\SKSE\`, so **the two live in different directories** —
   which is exactly how you lose ten minutes looking in the wrong one.

   Harmless (logging works), but it is a real data point for the fork migration in
   `docs/COMMONLIB.md`: worth re-checking against alandtse/CommonLibSSE-NG v7.0.0.

## Working style

State uncertainty explicitly rather than asserting. Verify against headers or the running
system rather than from memory — several of the notes above exist because an assumption was
wrong. Prefer measuring to estimating.
