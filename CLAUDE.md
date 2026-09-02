# FollowerTactics — project context

A Dragon Age: Origins-style tactics system for Skyrim SE/AE followers: an ordered list of
`IF <condition> THEN <action> ON <target>` rules, per follower, editable in game.

Read `docs/PLAN.md` first. `docs/RESEARCH.md` has the sourced findings behind it, with
explicit uncertainty flags.

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
| MO2 instance | `MO2/` inside this repo (gitignored) — mods at `MO2\mods` |

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

**Phase 0 — COMPLETE** (verified 2026-09-01 16:55). `FollowerTactics loaded (core
self-check: ok)` appeared in the `~` console, with the matching log:

```
[16:55:34.753] [info] FollowerTactics starting up
[16:55:41.894] [info] core self-check: rule 0 fired=true
```

**Phase 1 — current.** The go/no-go risk spike: one hardcoded rule — follower health < 50%
→ drink the best health potion, at most once every 10s — proving follower identification,
the tick loop, actor value reads, inventory scan, and reliable potion consumption. This is
where `src/game/` gets written; it is empty today. Reference implementation:
github.com/muenchk/NPCsUsePotions. Confirmed API:
`RE::ActorEquipManager::GetSingleton()->EquipObject(actor, alchemyItem, ...)`.
Per `docs/PLAN.md`: **if this takes more than a week, stop and reconsider the project.**

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
