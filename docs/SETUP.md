# Phase 0 — environment setup

## Current state (surveyed 2026-09-01)

Read directly off the machine, not assumed:

| | |
|---|---|
| `SkyrimSE.exe` | **1.7.104.0**, dated 27 Aug 2026 — the newest build |
| `CreationKit.exe` | **1.7.99.0**, dated 21 Aug 2026, installed **in the game folder** |
| SKSE | not installed |
| CKPE | not installed |
| Mod Organizer 2 | not installed |
| Plugins | 80 (`.esm`/`.esl`/`.esp`), of which 65 are Creation Club `.esl` — full AE content |
| Papyrus compiler | present at `Papyrus Compiler\PapyrusCompiler.exe` |
| Papyrus sources | already extracted to **`Data\Source\Scripts\`** |
| `bAllowMultipleMasterLoads` | **already set to 1** |
| `[Papyrus]` section of `CreationKit.ini` | present but **empty** — CK is on built-in defaults |

Good news: a clean vanilla base with no mod manager and no half-configured mods is the
ideal starting point. Bad news: **Steam has been auto-updating you** (game 27 Aug, CK
21 Aug), so blocking that is not optional.

Ordering matters. **Do not download SKSE first** — SKSE builds are locked to a runtime, so
the downgrade has to happen before you pick a version, or you'll fetch the wrong one.

## Step 1 — downgrade to 1.6.1170

You are on 1.7.104.0 and need 1.6.1170. The old Unofficial Downgrade Patcher is
discontinued; current options:

- [SDT — Skyrim Downgrade Tool](https://www.nexusmods.com/skyrimspecialedition/mods/188916) — downgrades from any version to 1.6.1170
- [Reliquary](https://www.nexusmods.com/site/mods/2188) — the successor the old patcher's author points to

Then **block Steam from re-updating**: set the game to "Only update this game when I launch
it" and always launch through MO2 / the SKSE loader, never through Steam. Belt and braces:
make `steamapps\appmanifest_489830.acf` read-only.

Verify afterward — right-click `SkyrimSE.exe` → Properties → Details → File version should
read `1.6.1170.0`. This is the one step where a silent failure costs you a day of confusing
crashes later.

### About the Creation Kit's version

**Correction to earlier advice in this file:** I first said to leave the 1.7.99 CK alone.
Downgrade it too, to **1.6.1378.1**, using the
[Creation Kit Downgrade Patcher](https://www.nexusmods.com/skyrimspecialedition/mods/190110).
The CK is a separate Steam app (appid **1946180**) with its own `appmanifest_1946180.acf`
and its own update cycle, so it needs its own update block as well.

Full procedure, backup list, and the objective verification check are in
[docs/DOWNGRADE.md](DOWNGRADE.md). Run `python tools/check_install.py "<Skyrim folder>"`
before and after.

## Step 2 — SKSE **2.2.8**, not 2.3.1

| Runtime | SKSE build |
|---|---|
| 1.7.104 (what you have now) | 2.3.1 |
| **1.6.1170 (our target)** | **2.2.8** |

2.2.8 is a bugfix release over 2.2.6; both target 1.6.1170, so take 2.2.8.

Get it from the [SKSE64 Nexus page](https://www.nexusmods.com/skyrimspecialedition/mods/30379)
under **Files → Old Files**. `ianpatt/skse64` on GitHub only goes to 2.2.6, so GitHub is not
a route to 2.2.8. silverlock.org keeps an archive at
`https://skse.silverlock.org/download/archive/` — I couldn't reach it to confirm filenames.

**Trap on the Files tab:** a **2.2.6 listed as a Main File targets 1.6.1179 (GOG)**, not
1.6.1170 (Steam). Same version number, different build. Take the Old Files one.

Install:

- `skse64_loader.exe` + `skse64_*.dll` → **game root**, next to `SkyrimSE.exe`
- `Data/Scripts/` → package as its own **MO2 mod**
- Add `skse64_loader.exe` as an MO2 executable and launch through it, always

Verify: SKSE's version appears in the **bottom-left of the main menu**. Console
`GetSKSEVersion` also works. If neither shows, runtime and SKSE don't match — recheck Step 1.

## Step 3 — Address Library

[Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444),
main file **"All in One (1.7.104.0) v13"**. Despite the name it covers all AE builds
including 1.6.1170. Do **not** take the old-gen 1.5.97 file.

## Step 4 — Mod Organizer 2

Not installed yet. Install it, point it at this Skyrim instance, and make a **dedicated dev
profile** with a deliberately minimal load order:

```
SKSE Scripts
Address Library
SkyUI
SKSE Menu Framework
FollowerTactics       <- our build output lands here
```

Nothing else. Every extra mod is a variable in every future bug report you write to yourself.
Keep this profile separate from any profile you actually play on — dev saves get corrupted on
purpose (see the script-baking note below).

Set the `SKYRIM_MODS_FOLDER` environment variable to MO2's `mods` folder. The CMake build
reads it and drops the built `.dll` straight into `mods\FollowerTactics\SKSE\Plugins\`, so a
rebuild is immediately live.

## Step 5 — Creation Kit config

Already installed, and `bAllowMultipleMasterLoads=1` is already set. Two things remain.

**CKPE** — [Creation Kit Platform Extended](https://github.com/Perchik71/Creation-Kit-Platform-Extended),
dropped next to `CreationKit.exe`. Crash fixes, raised internal record limits, render window
fixes. Take the build matching **CK 1.7.99**, and pick AVX2 or NoAVX2 to match your CPU.

**The Papyrus source path.** Your sources are extracted at `Data\Source\Scripts\` — the
Fallout 4 / Skyrim LE layout. Much community documentation says SSE's compiler wants
`Data\Scripts\Source\` instead, and recommends:

```ini
[Papyrus]
sScriptSourceFolder=".\Data\Scripts\Source"
```

Your `[Papyrus]` section is currently empty, so the CK is running on built-in defaults.
**Don't change anything yet** — first compile one trivial script and see whether it works as
shipped. Only if compilation fails to resolve imports do you need to either set that ini key
or mirror the sources into `Data\Scripts\Source\`. This path confusion is a known, unfixed
mess ([papyrus-lang#95](https://github.com/joelday/papyrus-lang/issues/95)), and guessing at
it before you've seen a real error just adds a second broken configuration.

**We do not need the CK yet.** The Phase 1 test scenario is entirely console-driven — see
`docs/TESTING.md`. The CK becomes necessary in Phase 4, for the ESP's combat styles,
abilities, and AI packages.


## How to launch things, from now on

The downgrade is only as durable as your launch habits. Both apps are pinned by a read-only
Steam manifest, but a manifest lock is a barrier, not a guarantee — the reliable protection
is never giving Steam a reason to check.

| What | How | Never |
|---|---|---|
| Skyrim | `skse64_loader.exe`, or MO2's Run button | The Play button in the Steam Library |
| Creation Kit | `CreationKit.exe`, ideally as an MO2 executable | Launching it from the Steam Library |

**Steam still has to be running.** Skyrim uses Steam DRM and will not start without it — you
are bypassing Steam's *launcher*, not Steam itself. Leave the client running in the
background and start the executables directly.

Why it matters, concretely: with Automatic Updates set to "Wait until I launch the game",
launching *through Steam* is precisely the moment Steam applies a deferred patch. The setting
does not prevent the update; it schedules it for that click. The Creation Kit is a separate
app (1946180) with the same behaviour, which is why it gets the same rule.

Once MO2 is installed, register both as MO2 executables and launch everything from there.
That gets you the virtual filesystem for free, and means you never think about this again.

If something does slip through, `python tools/check_install.py "<game folder>"` names the
exact files that changed.

## Step 6 — C++ toolchain

VS2022 + CMake + vcpkg. The repo is already scaffolded — `CMakeLists.txt`,
`CMakePresets.json`, `vcpkg.json`, and `vcpkg-configuration.json` are in place, with
baselines from
[SkyrimDev/HelloWorld-using-CommonLibSSE-NG](https://github.com/SkyrimDev/HelloWorld-using-CommonLibSSE-NG)
(a known-good pairing).

### Verified state on this machine (2026-09-01)

| | |
|---|---|
| Visual Studio | 2022 Community 17.12.4 |
| MSVC toolset | 14.42.34433 (`cl` 19.42.34436) |
| Windows SDK | 10.0.22621.0 |
| Extras | AddressSanitizer, clang/clang-tidy, CMake 3.29 + Ninja (VS-bundled) |
| vcpkg | `C:\vcpkg`, bootstrapped |

**Visual Studio was initially installed without the C++ workload.** That is the default
for a plain Community install, and it is worth calling out because it does not announce
itself: there is no `VC\Tools\MSVC` directory, no `vcvars64.bat`, no CMake, and no Ninja,
and the symptom is `cl.exe not found`, which reads like a PATH problem. If you are ever
rebuilding this environment, install the workload with VS **closed**:

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe" modify `
    --installPath "C:\Program Files\Microsoft Visual Studio\2022\Community" `
    --add Microsoft.VisualStudio.Workload.NativeDesktop `
    --add Microsoft.VisualStudio.Component.VC.ASAN `
    --add Microsoft.VisualStudio.ComponentGroup.NativeDesktop.Llvm.Clang `
    --includeRecommended --installWhileDownloading --quiet --norestart
```

Three traps in that one command, all of which fail quietly:

- `--installPath` must be **one already-quoted token**. Otherwise the installer's own
  parser splits on the space and reports `installPath: C:\Program`.
- `setup.exe` does not accept `--wait` (that belongs to `vs_installer.exe`); passing it
  exits 87 having done nothing.
- With Visual Studio open, the modify aborts with exit code **8006**,
  `Pre-check verification failed with warning(s): VSProcessesRunning` — and the only place
  that is visible is `%TEMP%\dd_installer_*.log`.

Afterwards, `vswhere -latest -requires ...` may still report nothing: a `--norestart`
install leaves the instance flagged `isComplete: False`, which hides it from `-latest` and
empties its package list even though the compiler works. Do not trust that signal; check
for `vcvars64.bat` on disk.

### Building

```powershell
.\tools\build.ps1 -Preset core -Test   # fast loop, no Skyrim, no vcpkg
.\tools\build.ps1 -Preset debug        # the SKSE plugin
```

`tools\build.ps1` imports the developer environment itself, so **no "x64 Native Tools
Command Prompt" is required**. It also restores `VCPKG_ROOT` afterwards — `vcvars64.bat`
overwrites it with the vcpkg bundled inside Visual Studio, which is not the one the
build was bootstrapped against and makes the build fail somewhere that looks unrelated.

The first `debug` configure is slow: vcpkg compiles fmt, spdlog and the rest, and the
first build compiles CommonLibSSE-NG from the submodule. Later builds are fast.

**On auto-deploy and elevation.** The build copies the finished `.dll` to
`$SKYRIM_MODS_FOLDER` (set to `MO2\mods`, inside this repo). Pointing `SKYRIM_FOLDER` at
the Steam install under `C:\Program Files (x86)` instead would make the post-build copy
require Administrator; the MO2 route avoids that entirely and is the better setup.

**Exit criterion for Phase 0:** launch through SKSE and see
`FollowerTactics loaded (core self-check: ok)` in the `~` console, plus a matching line in
`%USERPROFILE%\OneDrive\Documents\My Games\Skyrim.INI\SKSE\FollowerTactics.log`.

**That path is doubly counter-intuitive, and both halves are verified.** Documents is
redirected to OneDrive on this machine, and the subfolder is `Skyrim.INI` rather than
`Skyrim Special Edition` — CommonLibSSE-NG reads that folder name from a relocated global
in the game binary, and on 1.6.1170 it resolves to the wrong string. SKSE64's own
`skse64.log` still lands in `My Games\Skyrim Special Edition\SKSE\`, so the two are in
different directories. See the "Where the log actually is" section of `CLAUDE.md`.

**Status: Phase 0 met on 2026-09-01 16:55.**

## Already done for you

- The four console batch files are copied to the game root (`ftsetup.txt`, `ftspawn.txt`,
  `fthurt.txt`, `ftclean.txt`). See `docs/TESTING.md`.
- One stray zero-byte file, `_ft_writetest`, is sitting in the game root from a
  write-permission probe. Harmless — delete it whenever. I can't remove files on your
  machine without a deletion prompt, and it wasn't worth spending one.

## A note on dev saves

Papyrus bakes *script instance state* into save files. Changing a script's **shape**
(properties, states, function signatures) and then loading an old save leaves orphaned
instances and unreliable results. Rule of thumb: changing function bodies only → reloading is
fine; touching a script's public shape → start a fresh save. Keep a "clean dev save" parked in
the test cell and expect to remake it periodically.
