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
3. Lydia is a **persistent placed reference** (`000A2C94`), so `prid 000A2C94` targets her
   deterministically from a batch file without any RefID guessing. That single fact is what
   makes a scripted, repeatable scenario possible from the console.

If you later want a purpose-built arena,
[Spawn Arena Modders Resource](https://www.nexusmods.com/skyrimspecialedition/mods/168219)
adds one (`coc EEarena`) with spawn markers and group-spawn activators. Not needed for
Phase 1.

## Usage

```
coc QASmoke          <- type this manually, NOT in a batch file
bat ftsetup          <- Lydia: teleported, made a teammate, given potions
bat ftspawn          <- one hostile draugr
bat fthurt           <- drop her to a test threshold
bat ftclean          <- reset between runs
```

**`coc` must not go in a batch file.** Community reports say it either silently fails or
freezes the game. Travel first, then batch.

`QASmoke` is Bethesda's item-test room — isolated and free of random encounters, but
cluttered and not really an arena. Fine for Phase 1.

## What each file does

- **ftsetup** — `prid` Lydia, enable/resurrect (so reruns work), `moveto player`,
  `setrelationshiprank player 4`, `setplayerteammate 1`, add to `CurrentFollowerFaction`
  (`0005C84E`), give 10 Potions of Minor Healing + 5 Potions of Healing, print her health,
  and put the player in god mode.
- **ftspawn** — one draugr (`000387C0`), then re-selects Lydia. Re-runnable to escalate.
- **fthurt** — `forceav health <n>` on Lydia. **`forceav` sets current health without
  touching max/base**; `setav` and `modav` move max health and would corrupt the very
  percentage the rules engine reads. Edit the number to ~40% of whatever `getavinfo health`
  reported.
- **ftclean** — resurrect, restore health, un-teammate, strip inventory, god mode off.
  Does not delete spawned draugr; their RefIDs aren't knowable from a batch.

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
