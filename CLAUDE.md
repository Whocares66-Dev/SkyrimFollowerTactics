# FollowerTactics — project context

A Dragon Age: Origins-style tactics system for Skyrim SE/AE followers: an ordered list of
`IF <subject>: <condition> THEN <target>: <action>` rules, per follower, editable in game.

Read `dev/PLAN.md` first. `dev/RESEARCH.md` has the sourced findings behind it, with
explicit uncertainty flags. `dev/PRINCIPLES.md` is how code that meets the engine is written here: each rule, the incident that taught it, and where it is applied; read it before writing a feature that touches the game. `dev/MAGIC.md` is how casting works and what does not;
`dev/COMBAT_AI.md` is the combat AI: how it scores and chooses what to hold and cast (the score, the category order, each caster's gates, dual casting, read from the executable 2026-09-22), the combat style that tunes it, and what we change for a follower's weapons and attack spells: perks and immunities in the score, a held weighted-random pick with a recency penalty, and their spells standing down while a rule waits to cast (built 2026-09-22 on `wip-scoring`, not yet verified in play);
`dev/PROFILES.md` is how tactics live in the save (format, when, versioning);
`dev/UNIQUE.md` is how one copy of an item is told from another (the engine's
unique id, what a pin, a ban and a rule name, what is still to verify);
`dev/GAME_MODEL.md` is the plan for a model of the engine the item logic can be
tested against without Skyrim (not yet built; what is known, what to measure first);
`dev/DEVBENCH.md` is what devbench, a plugin that lets a script or an agent drive a running Skyrim, would give us, its limits, and how it meets the game model (observations; nothing wired in);
`dev/LOGGING.md` is the log design (levels, structured events, the JSON-lines sidecar);
`dev/EVENTS.md` is the game events: what tactics record, the last 1,000 kept in memory for the panel, and the per-session files and their archive (built 2026-09-14; not yet verified in play);
`dev/MODIFIERS.md` is where a follower's bonuses come from and how to total them (perk
entry points over actor values; research and thoughts, with the open questions);
`dev/VERSIONS.md` is every address, vtable slot and layout taken from the game rather than from CommonLib, each an (SE, AE) pair named in `src/game/Addresses.h` and read on 1.5.97 and 1.6.1170, and how to check them against another build; add a row and a name when you add one;
`dev/PLAYER.md` is the player under tactics: what carries over, how a cast is performed on the player's body through a synthesized press (the attack and shout handlers as read from the executable), what running rules out of combat needs, and what is verified in play and what is not (built 2026-09-18 on `wip-player-tactics`);
`dev/TESTING.md` is what of `src/game` is tested without Skyrim and how, what is still to extract, and the console harness for play;
`dev/I18N.md` is the panel in other languages: how SKSE mods localise, the `Tr`/`TrFormat`/`N_` marks, the JSON catalogs in `assets/Translations` and `tools/i18n.py` that keeps them in step, and fonts (built 2026-09-22 on `wip-i18n`; not yet seen in game);
`dev/TODO.md` is what is still to do.

**Progression** (folded in from its own repository 2026-09-21, branch `wip-progression`): followers level as the player does -- skills rise by use under the player's own rules read live from the game, levels bring perk and attribute points the player assigns, skills can be moved or reset for free, spell tomes can be taught -- and nothing is written to a record or the actor: skills, attributes, perks and spells are views in front of the engine. The code is `src/progression/core` (`fp_core`, RE-free, tested by `tests/progression`, `fp_tests`) and `src/progression/game`, in namespace `fp`; its pages are a follower's own (the skill page, the Character tab's attribute controls, Learn and Forget) and its switch is *Manage follower progression* in the Settings page's Progression section, free to flip because everything it does is reversible. `dev/PROGRESSION.md` is the design, `dev/POC.md` what is built and verified, `dev/ENGINE_PERKS.md`, `dev/ENGINE_SPELLS.md` and `dev/ENGINE_SKILLS.md` the engine behind the views and the learning hooks, `dev/PROGRESSION_README.md` the overview, and `dev/BRAINSTORM.md`, `dev/DESIGN.md`, `dev/PRIOR_ART.md` and `dev/research/` the research before it. It is pre-release: its co-save records carry no compatibility with earlier builds.

## The one architectural rule

**No `RE::` type may cross into `src/core/`.** There is no headless test harness for
Skyrim, so the only route to automated tests is logic that does not need the game.
`src/core/` (Snapshot, Rule, Evaluator) compiles with no Skyrim, no SKSE, no CommonLibSSE
and is covered by Catch2. `src/game/` holds every `RE::` call and is thin, imperative, and
verified by playing. `src/fix/` is `RE::` code too, kept apart: corrections to the engine's own behaviour, always on, for every actor, with no setting -- a staff the combat AI drops at half the charge it can still cast, two cloaks cast in turn forever -- where `src/game/` is the mod's features. `dev/COMBAT_AI.md` has what each fixes. If you want to `#include "RE/Skyrim.h"` in `core/`, the code belongs in
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
or the CK from the Steam Library** — use `skse64_loader.exe` or MO2. See `dev/DOWNGRADE.md`.

## Building

Use `tools\build.ps1` from any shell. It locates the VS install, imports `vcvars64.bat`
into the session, and then runs CMake -- so the "x64 Native Tools Command Prompt" is no
longer a prerequisite. (`CMakePresets.json` still pins `CMAKE_CXX_COMPILER=cl.exe`, so a
bare `cmake --preset` outside that environment still fails; the script is the supported
entry point.)

```powershell
.\tools\build.ps1 -Preset core  -Test      # rule engine + tests. Seconds.
.\tools\build.ps1 -Preset debug            # SKSE plugin. First run builds CommonLibSSE-NG.
.\tools\build.ps1 -Preset core-asan -Test  # same core tests, under AddressSanitizer
.\tools\build.ps1 -Preset core-cov -Coverage  # same, under clang-cl; which lines the tests reach
.\tools\build.ps1 -Preset debug -Analyze    # any preset, under MSVC's static analyser
```

Every build leaves two of the machine's cores free, so it does not take the desktop with it: ninja's own default is the core count plus two. `-Jobs <n>` overrides it, and the level is exported as `CMAKE_BUILD_PARALLEL_LEVEL`, so a `cmake --build` afterwards in the same shell -- the `tidy` and `format` targets -- takes the same limit.

`-Fresh` wipes the preset's build directory first. `-Analyze` compiles our targets with `/analyze` (findings are
C6xxx warnings; several times slower; the cached flag recompiles our sources on the
way in and out). `-Coverage` needs `core-cov`, runs the tests once and prints per-file
line coverage of `src/core`, with the line-by-line HTML in `build\core-cov\coverage\html`.

`core` / `core-asan` / `core-cov` need no vcpkg and no Skyrim at all -- that is the fast
feedback loop. `debug` / `release` build the plugin and pull CommonLibSSE-NG through vcpkg.
Every tool here ships inside the VS install: `clang-cl`, `llvm-cov`, `clang-tidy` and the
sanitizer runtimes are the "C++ Clang tools for Windows" component. ASan on MSVC finds
memory misuse, not leaks (LeakSanitizer has no Windows build); UBSan is clang-only and
its runtime is present, unused so far.

`.\tools\package.ps1` builds the release plugin once and writes TWO mod roots to `dist/` (a DLL, an ini, the repository's own README.md and the LICENSE each, installable from the archive in Mod Organizer; the test zip adds a TEST-BUILD.txt): `follower-tactics-<version>.zip` with `level = info`, what a player installs, and `follower-tactics-<version>-test.zip` with `level = debug`, ours to install here and never published. The DLL in them is the same file -- the log level is a runtime setting, and an MSVC debug build is not shippable at all, since it links a debug CRT nobody has -- and the test zip's ini is the player's with its level line rewritten, so the two cannot drift. The log's banner names the level it read, so an installed copy says which zip it came from. Packaging refuses if `assets/FollowerTactics.ini` is not on `info`. `dist/` is ignored.

`.\tools\release.ps1 patch|minor|major` cuts a release: bumps `project(... VERSION)` in CMakeLists.txt (the one place the version lives, and where the DLL's `FT_VERSION` comes from), runs the core tests and the release build, commits, tags `v<version>`, pushes, and makes the GitHub release with the player's zip attached -- **only** that one: the test zip stays in `dist/`, since a second download labelled "test" on the release page is an invitation to install the wrong one. A **tag** is git's name for a commit; a **release** is GitHub's object on top of one, and the only thing that can carry a built file. `-DryRun` says what it would do, `-Draft` leaves the release unpublished, `-Force` skips the clean-tree and remote ancestry checks -- but never the branch check: **a release is only ever cut from master**. Below 1.0 it is marked prerelease. Needs the GitHub CLI, logged in (`gh auth login`).

For a version already set in CMake, run `.\tools\release.ps1 none`; the script publishes that version's `CHANGELOG.md` section as its release notes. See [dev/RELEASING.md](dev/RELEASING.md) for the complete branch, verification, and recovery procedure.

**A build copies the DLL nowhere.** To try a change in game, run `.\tools\package.ps1` and install `dist\follower-tactics-<version>-test.zip` in Mod Organizer as any other mod -- the same archive a tester gets. Until 2026-09-18 a build landed the DLL straight in a mod folder named by `SKYRIM_MODS_FOLDER`, which put one machine's layout in the build, made every plugin build refuse to run while the game was up (it holds that DLL open), and tested something no player installs. New mods appear **unticked** in MO2 -- tick it or the DLL never loads. (`tools\deploy-tests.ps1`, which copies the console `bat/` scripts into the game folders, is a different thing and stays.)

Last verified green under MSVC 19.42 (`core`, `core-asan`) and clang-cl 18 (`core-cov`), 2026-09-09. Counts -- how many cases, what percentage covered -- are deliberately not kept here: they move with every test added, and a number that goes stale in a week teaches you to distrust the page. Run the presets and read the numbers off them.

## Before every commit

Run these before each commit, not after every edit: while a change is in progress, run only what tells you whether it works. Build and package first, optimistically, when the user is waiting to try a change; the checks come after. Each one has already caught something real in this project:

```powershell
.\tools\build.ps1 -Preset core -Test        # 1. tests
cmake --build --preset core --target format # 2. formatters, rewrite in place: C++, Python, PowerShell, CMake
.\tools\build.ps1 -Preset debug             # 3. the plugin builds (tools\package.ps1's release build counts)
```

## Before a merge or a push

The linters and the sanitizer run once, before the branch lands on master or is pushed, not at each commit: a full pass of the plugin's lint is ten to fifteen minutes. What they find is fixed and folded into the branch commit it belongs to (a fixup commit and an autosquash rebase), before master moves.

```powershell
cmake --build --preset core --target tidy   # linters: src/core and tests, Python, PowerShell
cmake --build --preset debug --target tidy  # the linter over src/game, src/fix and src/plugin.cpp too
.\tools\build.ps1 -Preset core-asan -Test   # AddressSanitizer
```

The formatter and the linters need the developer environment, so run them from a shell where
`tools\build.ps1` has already imported it, or wrap them the same way it does.
The core `tidy` reads `.clang-tidy` at the repo root: the bugprone, performance, analyzer,
concurrency and misc groups, nothing stylistic; the file says what is excluded and
why, and names the one known false positive. It runs one clang-tidy per file, so
Ninja spreads them across cores and skips the files that have not changed: a full
pass is ~99 s, one touched file ~6.6 s, nothing changed ~3.6 s. Delete
`build\<preset>\tidy` to force a full pass. `cmake/Quality.cmake` has the
measurements behind that: the per-file cost is the checks walking CommonLibSSE's
inlined header bodies, and no filter avoids it.

The plugin's `tidy` is the slow one -- every `src/game` translation unit parses the whole of CommonLibSSE, which no filter avoids and which the `/Y-` below means clang cannot precompile once and reuse, and a change to a core header re-lints every file.

The formatter and the linters cover every language of ours: clang-format and clang-tidy for C++, ruff (`ruff.toml` says which rules and why) for Python, PSScriptAnalyzer on its default rules for PowerShell, through `tools\check-powershell.ps1`, and gersemi (`.gersemirc`) for CMake, which is formatted but not linted (`cmake/Quality.cmake` says why). All but the C++ pair find their files through git, what it tracks or would track, so a script in a new folder is not missed; C++ is everything under `src` and `tests`. A tool that is not installed is said when CMake configures, and its language skipped: `pip install --user ruff gersemi`, and `Install-Module PSScriptAnalyzer -Scope CurrentUser` under pwsh 7.

PSScriptAnalyzer's rule against `Write-Host` allows it inside a function whose verb is `Show`, so every script prints through the `Show-*` helpers in `tools\console.ps1`, dot-sourced at the top of each; a state-changing function declares `SupportsShouldProcess` and asks `$PSCmdlet.ShouldProcess` before it writes, which is what makes `-WhatIf` on the downgrade script honest.

**A green build is not a passing check.** Every one of these has caught a defect
that compiled perfectly: the tests caught a cooldown interaction that changed
behaviour silently, the formatter has caught hand-written code on nearly every
pass, and extending the linter to `src/game` found dead code within a minute.

**Do not report work as finished without running them** -- the per-commit ones
for a commit, all of them for a merge. "It compiles" is the weakest signal
available here -- the whole point of the `RE::`-free core is that there IS a real
check, so use it.

### The linter's blind spot, and how it hid

**Each preset's `tidy` lints what its own compile database covers, and no more**: the core presets lint `src/core`, `src/progression/core` and `tests` (with the two checks `tests/.clang-tidy` relaxes), and `src/game`, `src/fix` and `src/plugin.cpp` -- which appear only in the *plugin's* database -- are covered by `cmake --build --preset debug --target tidy`. The folders are named in `cmake/Quality.cmake`, so a new one is linted only once it is added there: `src/fix` was not, at first, and reported clean. Getting `src/game` covered needs two
flags that are easy to get wrong:

- `--header-filter`, this checkout's `src/` as an absolute path, keeps CommonLibSSE's thousands of header lines quiet while still checking ours. Until 2026-09-22 it named folders (`src.(core|game)`), and every finding in a `src/progression` header was dropped unseen.
- `--extra-arg-before=/Y-` disables the precompiled header. MSVC's `.pch` is not
  a format clang can read, and **without this clang-tidy fails outright** with
  `not a valid precompiled PCH file` -- while reporting zero findings, which
  looks exactly like a clean run. If tidy ever reports nothing on a file you
  know is messy, check it actually parsed.

Until 2026-09-09 `tidy` reached across presets instead, pointing `-p` at `build/debug` whenever that directory existed. That test is answered at CONFIGURE time, so it went stale in exactly the tree that most needs it -- a fresh clone, or a new worktree -- and clang-tidy then parsed `src/game` with guessed flags and buried the real findings under `no type named 'string_view' in namespace 'std'` and `inline variables are a C++17 extension` -- errors that look like a catastrophe in our own headers and mean nothing at all. It is the `.pch` blind spot wearing the opposite mask, a catastrophic-looking run rather than a clean-looking one, and it is answered the same way: **check that it actually parsed.**

## Commits and merging

Work on a feature branch, never master, and commit as you go, one coherent change per commit.

A branch lands on master as logical commits, one per feature or fix it carries: a later fix to a feature, an attempt the branch replaced and a docs follow-up are folded into the commit they belong to, and what the branch did separately stays separate. Never one squash of the whole branch, and never every work-in-progress commit as it stood. Before master moves, run the checks under "Before a merge or a push" and fold their fixes in, then check that the rebuilt history ends on the branch's own tree (`git diff <branch> <rebuilt>` is empty). Rewriting a master already pushed is a force push, with `--force-with-lease`.

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
- **Never call `ActiveEffect::GetTargetActor()` or `MagicTarget::GetTargetAsActor()`.** They return a pointer 0x98/0xA0 into the actor, and it crashed the game (2026-09-13). Use `target->GetTargetStatsObject()` and `As<RE::Actor>()`. `dev/COMMONLIB.md` has the evidence and history.

We are on **alandtse/CommonLibSSE-NG v7.5.1** as the submodule `extern/commonlibsse-ng`
(clone with `--recurse-submodules`). Its vcpkg dependencies are in our manifest.
`dev/COMMONLIB.md` has why the fork was chosen and the API shapes worth knowing.

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

`dev/PLAN.md` section 5 rated "forcing an NPC to drink a potion isn't reliable" as the
highest risk in the project, the one that would kill the marquee feature. It works, via
`ActorEquipManager::EquipObject` with NPCsUsePotions' parameters.

### Performance: measured, and no optimisation needed

**47 us average, 68 us worst case** per follower evaluation, including the inventory scan.
At the original 150 ms tick that was ~0.3 ms/second for one follower; eight followers
~2.6 ms/second, about **0.04 ms/frame amortised at 60 fps** against a 0.5 ms/frame budget.
The tick is now 500 ms (one decision per half-second "turn"), so the cost is lower still.

So the three things `PLAN.md` 3.2/3.3 called for -- staggered scheduling, cached expensive
sensors, dependency-driven sensor activation -- are **not needed** on a follower's bag. On the player's the number is different: **20 ms per evaluation** in Nordic Souls (2026-09-18), every half-second while a list waits or idle rules run. Reading the bag on demand was built and taken out the next day: it saved about a millisecond of the twenty. The cost line times each step of the snapshot (self, party, each other actor's traits, hands, spells, effects, the bag), and it named the step: **spells, 19 ms**, the engine's magicka cost calculation over every spell the player knows, twice for the dual cast. The snapshot now prices only the spells the actor's rules name (`dev/PLAYER.md` "Cost"). Logged every 5 s of evaluation, so drift is visible.

**Casting — WORKS end to end (2026-09-02 13:15).** `dev/MAGIC.md` "The eighth attempt":
UseMagic packages put at the front of the follower's own package stack, gated by a
condition held by a lease (a faction rank until 2026-09-08), released when the follower's own spell-fire animation event
names our spell. Measured over two cycles: rule fires at 43% health, package selected on
the same tick, `Fast Healing -- OURS` 1.4 s later, health 75 -> 175, released next tick,
follower back to fighting. Since 2026-09-08 the records are **made in memory** (`src/game/Forms.cpp`, `dev/MAGIC.md` "Forms at runtime"): no plugin
file, nothing of ours in the save, the DLL is the whole mod. Verified in play
the same day; the ESP is gone (git history before 2026-09-08 has it). Since 2026-09-14 they are one set per follower, made when the tick first sees them, instead of a shared pool of sixteen made at load; not yet verified in play.
Recruit through dialogue (or `cqf DialogueFollower
SetFollower`), never `setplayerteammate`. Cooldowns and leases run on game time.

**Phase 2 — in progress.** The rule engine has the subject/predicate model, a list of
actions per rule done one per tick, equip actions that pin, and conditions for status,
armour, resistance, attacked-by, the party's extremes and the player's fight
(`dev/CONDITIONS.md`); `Snapshot::allies` / `enemies` are populated by definition
(the party, and whoever the compass paints red). Rules persist in the SKSE co-save:
one JSON record per follower with the rules, the switch and the player's pins, written
when the game saves and taken back when the tick first sees the follower after a load
(`dev/PROFILES.md`; built 2026-09-04, not yet verified in play). Still missing:
shareable named profiles. Actions to come are in `dev/ACTIONS.md`.

**The player under tactics (built 2026-09-18 on `wip-player-tactics`; casts, dual casts, a shout, powers and the blows seen in play the same day).** The player is evaluated in a fight as a follower is, under rules of their own on a Tactics tab of their page, saved as a follower's are. Cast, Dual Cast, Scroll, Power and Shout are performed on the player's own body through the game's input handlers, by synthesized presses of the hand's attack controls or the shout control (`src/game/PlayerCast.cpp`, `dev/PLAYER.md`); the blows go by the engine's own attack actions; the equips are plain equips, and the player's snapshot carries what they wear where a follower's carries the pin book. Attack alone is left out. **Out of combat is a second list (built 2026-09-18 on `wip-idle-tactics`, not yet verified in play):** an Idle Tactics tab after Tactics, on a follower's page and the player's, the same editor over a list of its own, evaluated out of a fight as the combat list is in one; a list carries its moment (`RuleSet::moment`), one context serves both, and the idle list offers no Enemy, no Attacker, no Combat start or end, no Hit by, no Attack or blow, no Fleeing (`dev/PLAYER.md` "Out of combat"). Diseased is a status since the same day.

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
SKSE's own `skse64.log` is in the same folder. Each launch moves the previous launch's pair into `SKSE\FollowerTactics\` as `FollowerTactics-<start>_<end>.log` and `.events.jsonl`, in UTC and Crash Logger's format, and keeps the last 20 (`dev/EVENTS.md` "Sessions and files").

The first is prose to read while playing: every line, filtered by the level set in `Data/SKSE/Plugins/FollowerTactics.ini` — `info` by default, `debug` for the per-tick readouts. The second is the game events, what tactics did to and saw of a follower, one JSON object per line, whatever the level; a game event is one call that writes both. `dev/LOGGING.md` is the machinery, the levels and how to add a call site (`ft::log::<module>.info(...)`; there is no `logger` alias any more); `dev/EVENTS.md` is which events there are.

## Reading the executable

`tools/disasm.py <address-library-id>` disassembles a function from
SkyrimSE.exe; `--vtable <id>` dumps a vtable, `--lookup <rva>` names the
function an address falls in. `--version 1.7.104` first picks the build:
the exes live unpacked under `C:\Modding\SkyrimVersions\` (the installed
one is SteamStub-encrypted and reads as noise) and the database comes from
`AddressLibrary/`, any format; 1.6.1170 by default. Every ID it prints
carries the name CommonLib or our `src/game/Addresses.h` gives it
(`tools/names.py`), so naming an address there names it in every read
after; `--name <text>` finds IDs by name. Ghidra 12.1.4 has each build
analysed under `C:\Modding\SkyrimVersions\ghidra`, and
`tools/ghidra-mcp.ps1 [-Version <build>]` serves one over MCP to the servers
`.mcp.json` names, every name above imported first: use it to decompile and
cross-reference; `disasm.py` keeps IDs, matching across builds and the live
process. `tools/addrlib.py --all
<ids>` looks IDs up in every build's database at once. While the game is
running, `tools/livedisasm.py` reads the decrypted code out of the live
process instead, with the same modes plus `--callers <id>` (every call into
a function) and `--bytes`. `dev/VERSIONS.md` "Builds on disk" is which
builds there are and how to get another; `dev/MAGIC.md` "Forms at runtime"
and `dev/UNIQUE.md` are what has been read with them so far.

## Reading a save

`tools/ess_scan.py <save> npc <form id>` decodes an NPC base record's saved sections straight from a save file, no game needed; `scan` looks for bytes that match our runtime form IDs. Its docstring has what is known of the format and how far to trust it. It found the `addshout` save crash (2026-09-14).

## Fetching UESP / Nexus pages

`WebFetch` is blocked on `uesp.net` (both subdomains) and `nexusmods.com`. Don't retry it or spoof curl headers at these -- use, respectively, the `search-uesp` skill, the `search-creation-kit-wiki` skill, and houseCARL's keyless Nexus tools (`housecarl_nexus_*`, see its MCP instructions) for the verified working methods.

## Working style

State uncertainty explicitly rather than asserting. Verify against headers or the running
system rather than from memory — several of the notes above exist because an assumption was
wrong. Prefer measuring to estimating.
