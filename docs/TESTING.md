# Test harness

The Phase 1 scenario — player + follower + hostile monster, follower drinks a potion at low
health — needs **no Creation Kit work at all**. It's four console batch files.

Files live in `test/`. Copy them to the **Skyrim root**, next to `SkyrimSE.exe`.
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

## Usage

```
coc QASmoke        <- type this manually, NOT in a batch file
bat ftsetup        <- god mode on, spawn a follower copy in front of you
                      *** now CLICK her in the console to select her ***
bat ftmake         <- make the SELECTED actor a teammate and give her potions
bat ftspawn        <- spawn one hostile draugr
bat fthurt         <- drop her to 25 HP; watch this one happen
bat ftstatus       <- print the selected actor's state
bat ftclean        <- reset between runs
```

The click in the middle cannot be automated: no console command selects the reference
`placeatme` just created. Everything after `ftsetup` therefore acts on the **current
console selection**, so it works on any actor you click.

| Script | Acts on | Does |
|---|---|---|
| `ftsetup` | player | `player.tgm`, then `player.placeatme 000A2C8E 1` — spawns a disposable copy of Lydia from her **BaseID** |
| `ftmake` | selection | `setplayerteammate 1` (the `kPlayerTeammate` bit the mod's registry keys on), relationship rank, `CurrentFollowerFaction`, 10 Minor Healing + 5 Healing potions, then prints health and potion count |
| `ftspawn` | player | one draugr (`000387C0`). Re-runnable to escalate |
| `fthurt` | selection | prints health, `forceav health 25`, prints health again. **`forceav` sets current health without touching max/base**; `setav`/`modav` move max health and would corrupt the very percentage the rules engine reads |
| `ftstatus` | selection | health/magicka/stamina, `isincombat`, `getcombattarget`, potion counts — answers "is the scenario actually set up?" |
| `ftclean` | selection | resurrect, un-teammate, strip inventory, then `disable` + `markfordelete` the copy, and god mode off |

**If a script appears to do nothing, you probably have nothing selected.** The console shows
the selected reference's name and FormID just above the input line — check it says an NPC.
That failure mode is silent and looks identical to a broken batch file.

**Deploying them.** `bat <name>` does not read this repo. `tools\deploy-tests.ps1` writes
each file to all three places the console might look — game root with `.txt`, game root
without an extension, and `Data\` with `.txt` — and converts to **CRLF** on the way.
Both of those mattered: LF-only files in the root did nothing at all.

```powershell
.\tools\deploy-tests.ps1            # copy repo -> game (3 locations, CRLF)
.\tools\deploy-tests.ps1 -Check     # report what is present
```

The game folder is under `C:\Program Files (x86)`, so deploying needs an elevated shell.

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
