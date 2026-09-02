# FollowerTactics

A Dragon Age: Origins-style tactics system for Skyrim SE/AE followers — an ordered list of
`IF <condition> THEN <action> ON <target>` rules, per follower, editable in game.

Status: **Phase 0 complete** (2026-09-01). The DLL builds, deploys, loads under SKSE at
runtime 1.6.1170, and the rule engine runs inside the game — verified by console line and
log. It does not yet touch a single follower: Phase 1 is where `src/game/` gets written.

## Docs

| | |
|---|---|
| [docs/PLAN.md](docs/PLAN.md) | Architecture, phases, risks. Read first. |
| [docs/RESEARCH.md](docs/RESEARCH.md) | Sourced findings on what the engine does and doesn't allow, with uncertainty flags. |
| [docs/SETUP.md](docs/SETUP.md) | Phase 0 environment. **Order matters — downgrade before installing SKSE.** |
| [docs/COMMONLIB.md](docs/COMMONLIB.md) | What CommonLibSSE is, which fork is actually maintained, and why we're on the one we're on. |
| [docs/TESTING.md](docs/TESTING.md) | Console-driven test scenario. No Creation Kit needed. |

## Layout

```
src/core/     the rule engine — NO RE:: types, ever. This is the tested part.
src/game/     everything that touches Skyrim. Thin, imperative, verified by playing.
src/plugin.cpp SKSE entry point
tests/        Catch2. Runs with no Skyrim, no SKSE, no CommonLibSSE.
test/         console batch files for the in-game scenario
profiles/     JSON rule sets
esp/          the ESL-flagged plugin (Phase 4)
papyrus/      .psc sources (thin)
```

The line between `src/core/` and `src/game/` is the most important rule in this repo.
There is no headless test harness for Skyrim, so the only way to get automated tests is to
make the interesting logic not need the game. If you want to `#include "RE/Skyrim.h"` in
`core/`, the code belongs in `game/`.

## Building and testing

Everything goes through `tools\build.ps1`, which locates Visual Studio, imports
`vcvars64.bat` into the session, and then runs CMake. You do **not** need to open an
"x64 Native Tools Command Prompt" — that requirement is the single most common source of
a confusing `cl.exe not found`.

```powershell
.\tools\build.ps1 -Preset core -Test       # the fast loop: rule engine + tests, seconds
.\tools\build.ps1 -Preset core-asan -Test  # same, under AddressSanitizer
.\tools\build.ps1 -Preset debug            # the SKSE plugin
```

| Preset | Needs | What it builds |
|---|---|---|
| `core` | a C++23 compiler | `ft_core` + Catch2 tests. No Skyrim, no SKSE, no vcpkg. |
| `core-asan` | same | same, with AddressSanitizer |
| `debug` / `release` | vcpkg, CommonLibSSE-NG | the above **plus** the SKSE plugin DLL |

`core` fetches Catch2 itself if vcpkg isn't providing it, so it works on a bare machine.
Add `-Fresh` to wipe the build directory first.

Last verified: 10 test cases / 32 assertions, green under MSVC 19.42 and gcc 13.3.

Portable route, no CMake at all:

```sh
g++ -std=c++23 -Wall -Wextra -Isrc -c src/core/Evaluator.cpp -o Evaluator.o
g++ -std=c++23 -Isrc tests/test_evaluator.cpp Evaluator.o -lCatch2Main -lCatch2 -o ft_tests
./ft_tests
```

### The plugin

Windows, VS2022 with the **Desktop development with C++** workload, and vcpkg.
`SKYRIM_MODS_FOLDER` points at the MO2 `mods` folder, so a rebuild deploys the DLL to
`MO2\mods\FollowerTactics\SKSE\Plugins\`. A newly created mod appears **unticked** in
MO2 — tick it, or the DLL never loads.

Requires the Phase 0 environment: runtime **1.6.1170**, SKSE **2.2.8**, Address Library.
See [docs/SETUP.md](docs/SETUP.md). `python tools/check_install.py "<game folder>"`
verifies it and reports drift.

## Semantics

A rule reads:

```
IF <subject> <predicate> <arg>   THEN <action> ON <target>
   Enemy     HealthPctBelow 0.25      SetCombatStyle  ConditionSubject
```

Subject and predicate are **separate**, mirroring Dragon Age's cascading editor. Baking the
subject into the predicate name (`SelfHealthPctBelow`, `AllyHealthPctBelow`, …) multiplies
every new predicate by every subject and makes the UI's two columns secretly dependent.
`IsPredicateValidFor(subject, predicate)` is the single source of truth for which pairs mean
anything; the menu is built from it, so an impossible pair is never offered.

`Ally` and `Enemy` are **group** subjects. The member that best satisfies the predicate
becomes the rule's **binding**, and actions target it by default — so "enemy below 25% health
→ finish it" names the enemy once and the action lands on that same enemy. When several
members match, the binding follows the predicate's own dimension: health predicates bind the
weakest, everything else binds the nearest, ties resolved by snapshot order.

Rules are evaluated top to bottom every tick. The **first** rule whose condition holds and
whose action is actually available fires — then evaluation stops. If nothing fires, nothing
happens and the native combat AI carries on: an empty rule set behaves exactly like vanilla.
This is an override layer, never a replacement.

Every rule that doesn't fire records *why* (`condition false`, `on cooldown`, `no potion`,
`no target`, `unsupported`, `invalid condition`). That trace drives the UI's debug column,
which is what makes authoring rules against an opaque engine tractable. `invalid condition`
is deliberately distinct from `condition false`: "your rule is broken" and "your rule is fine
but the world isn't in that state" look identical from a rule that never fires, and they send
you to entirely different places to debug.
