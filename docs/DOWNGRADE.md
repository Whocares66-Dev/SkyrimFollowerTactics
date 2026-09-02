# Downgrading 1.7.104 → 1.6.1170

You are on **1.7.104.0** (verified from the PE version resource, not guessed). Target is
**1.6.1170**, the long-standing pre-Creations-Update AE build where the entire SKSE
ecosystem is stable.

Run `python tools/check_install.py "<your Skyrim folder>"` before and after. It reads the
real version out of the executable and diffs key file sizes against the baseline recorded
from your machine on 2026-09-01.

---

## First, a correction to what I told you earlier

I said the Creation Kit could stay at 1.7.99. **That was too casual.** The community ships a
[Creation Kit Downgrade Patcher](https://www.nexusmods.com/skyrimspecialedition/mods/190110)
that pairs CK **1.6.1378.1** with game **1.6.1170** — the existence of a purpose-built
matching downgrader is a clear signal they're meant to move together. No source I found
reports a hard failure from running a 1.7.99 CK against a 1.6.1170 game, so this is
"recommended" rather than "proven required", but the CK was built expecting the 1.7.x ESM/BSA
set and we don't need that risk.

Also worth knowing: **the CK is a separate Steam app** (appid **1946180**, vs the game's
**489830**). It has its own `appmanifest_1946180.acf` and its own update cycle. Downgrading
the game does not touch it, and freezing the game's updates does not freeze the CK's.

## Second: this is not an exe-only downgrade

I assumed earlier that only `SkyrimSE.exe` changes between builds. **Wrong.** The master
ESMs and several BSAs changed too — which is exactly why the official method pulls three
depots rather than one.

Your `Skyrim - Interface.bsa` is **106,921,497** bytes. Community figures put 1.7.99 at
106,921,425 and 1.6.1170 at 105,799,354. Yours matches neither — it's 72 bytes off the
1.7.99 number, consistent with 1.7.104 having touched it again. After a correct downgrade it
should read exactly **105,799,354**. That's your single best objective check.

Consequence: **avoid binary-patcher tools that claim to touch only the exe and DLLs.**
["Best of Both Worlds"](https://www.nexusmods.com/skyrimspecialedition/mods/169962) says it
patches only executables and "necessary files", which contradicts the evidence above. Its
description may just be underselling what it does — but for this particular jump, prefer a
method that replaces whatever actually differs.

---

## Back up first

None of these tools make an automatic backup, and Reliquary explicitly doesn't. Before you
start, copy these somewhere outside the game folder:

```
SkyrimSE.exe
SkyrimSELauncher.exe
Data\Skyrim.esm      Data\Update.esm      Data\Dawnguard.esm
Data\HearthFires.esm Data\Dragonborn.esm  Data\_ResourcePack.esl
Data\Skyrim - Interface.bsa
```

Steam's "verify integrity" can also restore you to 1.7.104, so this is belt-and-braces — but
it's only ~490 MB (measured on this machine) and it removes the only irreversible-feeling
step from the process.

---

## What a depot actually is

A Steam app isn't one blob — it's a set of **depots**, each a versioned bundle of files.
Skyrim SE has 11: three for the game itself, seven language audio packs, and one for the
VC++/DirectX redistributables.

A **manifest** is an immutable snapshot of one depot at one build: the file list, sizes, and
chunk hashes. The long number in each command is a manifest ID — it names one exact
historical state of that depot. Content is chunk-addressed and deduplicated across builds,
which is why a normal update downloads far less than the files' total size.

The closest analogy: `docker pull image@sha256:...` instead of `:latest`. Or in git terms,
a depot is a repo, a manifest ID is a commit hash, and `download_depot` is checking out an
old commit. Steam's UI only ever offers you the newest manifest per branch, but older ones
stay on the CDN — `download_depot` is the escape hatch that lets you name one directly.
That is the entire mechanism behind this downgrade: you are asking for the January 2024
snapshot instead of today's.

The three we need, with Valve's own names from [SteamDB](https://steamdb.info/app/489830/depots/):

| Depot | Name | On disk | Download |
|---|---|---|---|
| 489831 | Skyrim Special Edition **disk** | 6.98 GiB | 4.75 GiB |
| 489832 | Skyrim Special Edition **core** | 7.98 GiB | 7.10 GiB |
| 489833 | Skyrim Special Edition **exe** | 36.15 MiB | 12.97 MiB |

The exe lives in its own tiny depot so a code-only patch is a 13 MB download rather than 12
GB — which is also why "just patch the exe" tools exist, and why they're wrong for this
particular jump: 1.7.x touched files in all three depots, not only the exe. The
`Skyrim - Interface.bsa` size discrepancy is the proof.

File counts are **19 / 26 / 1** and sizes **4663 / 7260 / 26 MB**, as reported by the Steam
client itself while downloading these exact manifests on 2026-09-01. Note this corrects a
community source that claimed 16 files for depot 489831. The script uses these as sanity
checks against an interrupted copy — Steam's own "Depot download complete" line is the real
authority on whether the download succeeded.

Which specific BSAs sit in "disk" versus "core" is community-reported rather than something
I verified — it doesn't matter in practice, since we replace all three.

**Language packs:** we don't touch them. If you play in English this is irrelevant; the
English voice data is in the three depots above. If you use a non-English audio pack, that
depot stays at its current version — likely harmless, since voice BSAs rarely change, but
worth knowing it wasn't reverted.

---

## Scripted: `tools/Downgrade-Skyrim.ps1`

Most of this is automated. Run it from an **elevated** PowerShell (it writes into
Program Files). It finds Steam via the registry and `libraryfolders.vdf`, so normally you
don't pass any paths.

```powershell
cd C:\project\SkyrimFollowerTactics
.\tools\Downgrade-Skyrim.ps1 -Step check     # where you stand right now
.\tools\Downgrade-Skyrim.ps1 -Step backup    # ~2 GB, before touching anything
.\tools\Downgrade-Skyrim.ps1 -Step depots    # prints the exact lines to paste
#   ... paste those three into the Steam console, one at a time ...
.\tools\Downgrade-Skyrim.ps1 -Step install -WhatIf   # dry run, changes nothing
.\tools\Downgrade-Skyrim.ps1 -Step install           # copy depots into place + verify
.\tools\Downgrade-Skyrim.ps1 -Step lock              # read-only both appmanifests
```

`-Step restore` puts the backed-up files back; `-Step unlock` reverses the lock.

**What it will not do: download the depots.** `download_depot` exists only inside the Steam
client's own console and cannot be driven from a script. `-Step depots` prints the three
lines for you to paste, and also prints equivalent
[DepotDownloader](https://github.com/SteamRE/DepotDownloader) commands if you'd rather have
that part scripted too — that route needs your Steam login and 2FA.

Safety properties, all exercised in testing:

- **Never deletes anything.** Overwrites only, and it runs the backup automatically before
  the install step, whether or not you ran it yourself.
- **`-WhatIf` on every mutating step**, verified to leave the tree byte-identical.
- Refuses to install if Skyrim or the Creation Kit is running, or if all three depots
  aren't present, and warns if you're not elevated.
- Checks depot file counts (16 / 26 / 1) and flags an incomplete download.
- Skips files whose size already matches, so re-running is cheap and safe.
- After installing, re-runs `check` and **fails loudly on the partial-downgrade trap** —
  exe says 1.6.1170 but `Skyrim - Interface.bsa` doesn't, meaning only depot 489833 landed.

### How it was tested

Run against a synthetic Steam tree in a Linux container with PowerShell 7.4.6 — real file
sizes, correct depot file counts, and a crafted PE version resource. Full round trip
verified: `check` → `depots` → `install -WhatIf` (tree unchanged) → `install` → `check`
(Interface.bsa correctly reported as matching 1.6.1170) → `lock` → `restore` → `unlock`.
The partial-downgrade detection was tested by forging an exe reading 1.6.1170 while leaving
the BSA at the 1.7.104 size; it was caught.

Two real bugs surfaced and were fixed during that testing, both classic PowerShell traps:
indexing an `[ordered]` hashtable with integer keys (treated as a *positional* index, so
`$DEPOTS[489831]` threw), and `.Count` on the scalar that `Get-ChildItem` returns when a
folder holds exactly one file — which is depot_489833, the one with just the exe.

**Caveat worth stating plainly:** it has been tested against a *simulated* install, not your
real one, and never on Windows. The Windows-only paths — registry lookup, elevation check,
`Get-Process`, read-only attributes on NTFS — are written from documentation and are not
exercised by that test. Run `-Step check` and `-Step install -WhatIf` first and read what
they say before letting it write anything.

---

## Method A — Steam console depots (recommended)

The ground-truth method. No third-party tool, and you can see exactly what you're getting.

**1. Open the Steam console.** `Win+R` → `steam://open/console` → Enter. A "CONSOLE" tab
appears in the Steam client.

**2. Run these three commands ONE AT A TIME.** Paste the first, press Enter, wait for it to
report completion, then the next. Pasting all three at once is the most commonly reported
way to end up with a corrupt download.

```
download_depot 489830 489831 8442952117333549665
download_depot 489830 489832 8042843504692938467
download_depot 489830 489833 1914580699073641964
```

These manifest IDs for 1.6.1170 (released 17 Jan 2024) are confirmed identically across three
independent sources: the
[Nexus Steam manifest list](https://www.nexusmods.com/skyrimspecialedition/articles/6536),
the [Wildlander wiki](https://wiki.wildlandermod.com/09-How-Do-i/HowDoI/downgrade/), and a
Sept-2026 gist written about this exact 1.7.x problem. Expect roughly 15 GB total —
489831 is game data and ESMs (16 files), 489832 is interface and texture BSAs (26 files),
489833 is the executable alone (~35 MB).

**3. Copy the files into place.** Downloads land in:

```
C:\Program Files (x86)\Steam\steamapps\content\app_489830\depot_489831\
                                                          depot_489832\
                                                          depot_489833\
```

Copy the **contents** of each `depot_*` folder into
`steamapps\common\Skyrim Special Edition`, overwriting. Copying the folders themselves rather
than their contents is the other classic mistake here.

**4. Verify.** `python tools/check_install.py "<your Skyrim folder>"` — you want
`SkyrimSE.exe 1.6.1170.0` and `Skyrim - Interface.bsa` at 105,799,354.

## Method B — Reliquary

[nexusmods.com/site/mods/2188](https://www.nexusmods.com/site/mods/2188). A cross-game Steam
build manager. It logs into Steam properly, identifies your build by **hashing the exe**
rather than trusting Steam's manifest record, and does a delta download — only what differs,
often a fraction of the full 15 GB. This is the least error-prone option if you'd rather not
hand-type manifest IDs. Note it modifies in place with no backup.

## Method C — SDT

[Skyrim Downgrade Tool](https://www.nexusmods.com/skyrimspecialedition/mods/188916), v1,
20 Aug 2026. A PowerShell script launched from a `.bat`; per its description it automates the
same depot process as Method A. Extract it **outside** the game folder and run the `.bat`,
not the `.ps1`. Steam only, no GOG/Epic/Game Pass.

## Not Method D

The [old Unofficial Downgrade Patcher](https://www.nexusmods.com/skyrimspecialedition/mods/57618)
is discontinued, and recent users report it failing outright ("Can't patch file, it doesn't
match the expected format"). Its own author now points at Reliquary.

---

## Then downgrade the Creation Kit

Use the [Creation Kit Downgrade Patcher](https://www.nexusmods.com/skyrimspecialedition/mods/190110):
`CreationKit.exe` 1.7.99 → **1.6.1378.1**, the build that pairs with game 1.6.1170. It's a
BSDiff4 binary patch of that one file — no depot download, no script needed. **Back up
`CreationKit.exe` first**, since a binary patch has no undo of its own.

Its page lists the "Best of Both Worlds" game patcher as a prerequisite. That's about the
*game* already being downgraded, which ours is — by the depot route instead. The patcher
operates on `CreationKit.exe` 1.7.99, which is what we have.

**The real reason, and it's not what I first said.** I originally justified this as a
save-format risk. That was wrong, and worth correcting:

- **Plugin format is fine either way.** Form version has been 44 since SE launched in 2016.
  The header version jump 1.70 → 1.71 happened at game **1.6.1130**, in Dec 2023 — *older*
  than our 1.6.1170 target, which therefore already speaks 1.71 natively. A plugin saved by
  either CK carries the same header version.
- **The actual blocker is CKPE.** [Creation Kit Platform Extended](https://github.com/Perchik71/Creation-Kit-Platform-Extended)
  lists 1.5.73, 1.6.1130 and 1.6.1378.1 as supported. **1.7.99 is not supported at all.**
  Since CKPE is what makes the CK survive real work — crash fixes, raised internal record
  limits, render-window fixes — running unpatched 1.7.99 is the practical risk, entirely
  independent of file formats.

One thing to keep in mind regardless of which CK you run: the Creations Update added four
Papyrus natives — `SetContainerAllowStolenItems`, `GetAllItemsCount`, `IsContainerEmpty`,
`RemoveAllStolenItems`. They compile fine but don't exist on a 1.6.1170 runtime, so a script
calling them fails at run time, not compile time. None are anything this mod needs.

Verify afterward with `tools/check_install.py`, which now checks the CK version too.

## Block Steam from undoing all of it

Do all three. None is individually reliable any more.

**1. Update setting.** Library → right-click Skyrim SE → Properties → **Updates** →
**Automatic Updates** dropdown. Choose **"Wait until I launch the game"**.

**There is no "never update" option, and Steam has never had one.** The dropdown offers
exactly four choices — use global setting, wait until I launch, let Steam decide, and
immediately download — and "wait until I launch" is the most restrictive of them. It is the
current name for what older guides call "Only update this game when I launch it".

Understand what it actually does: it stops Steam **background-downloading** a patch. It does
not refuse the patch forever — it defers it until you launch the game *through Steam*. The
setting only becomes "never" when combined with the other half:

> **Never press Play in the Steam Library.** Launch through `skse64_loader.exe` or MO2, which
> starts the game directly and does not pass through Steam's update gate.

That pairing — deferred updates plus never launching via Steam — is the whole trick. Neither
half works alone. And because it depends on your own discipline rather than an enforced
setting, the read-only manifest in step 2 is what actually protects you.

This setting has also been reported to silently reset after Steam client updates, so recheck
it periodically.

That same properties page shows **Build ID** and *"Installed content updated"*, which is a
quick independent way to tell whether Steam has quietly re-patched you. Before the downgrade
this machine read Build ID **24914197**, updated 27 Aug 2026 — the 1.7.104 build. After a
successful downgrade it should read a January 2024 build, and if it ever returns to
24914197 you have been re-updated.

**2. Read-only manifest.** `C:\Program Files (x86)\Steam\steamapps\appmanifest_489830.acf`
(the `steamapps` root, not inside `common\`) → Properties → Read-only. Do the same for
`appmanifest_1946180.acf` to freeze the Creation Kit. Caveat: an August 2026 comment reports
this working for three days and then Steam updating anyway — treat it as a strong barrier,
not an absolute one.

**3. Re-check after every Steam client restart.** `check_install.py` takes two seconds. If
Steam ever gets to contact its servers with a writable manifest, it will silently overwrite
your downgraded files with no prompt beyond a normal download bar.

**Never run "Verify integrity of game files"** after this. It hash-checks against the latest
manifest and will re-download you straight back to 1.7.104. That's also your rollback path if
you ever want it: clear the read-only flags, set updates back to automatic, verify integrity.

---

## First launch after downgrading

Go online in Steam once and launch the game normally, so it can re-pull Creation Club
content — SDT's instructions call this out specifically, which implies some CC delivery
routes through Steam metadata on first launch post-downgrade. Then go back to the locked-down
configuration above.

Your 65 CC `.esl` files are ordinary Data-folder content and are not tied to the exe build.
1.6.1170 is itself a full Creations-capable AE runtime — it's the build Creations originally
shipped on — so nothing in your AE content requires 1.7.x.

## Order matters

Downgrade **before** installing SKSE. SKSE builds are runtime-locked; you want 2.2.8, and
that only makes sense once the game actually reads 1.6.1170.
