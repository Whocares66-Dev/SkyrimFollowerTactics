# Version-bound dependencies

What the plugin takes from one build of the game rather than from CommonLib or the game's data: what each is, what uses it, what happens on another runtime, and how far it was checked. Every entry here was read on **1.6.1170**.

**The plugin is built for both lines** (SE from 2026-09-18; AE only between 2026-09-17 and then). Every address of our own is one named `(SE, AE)` pair in `src/game/Addresses.h`, and nothing else names a bare ID: an Address Library ID is stable within a line but not across one, so on SE the same number names another function, and the SE and AE halves are each read on their own build. VR stays off: its library is a third one, of raw offsets, that nobody here can read or run.

A new dependency of this kind gets a row here and a name in `Addresses.h` when it is added.

## ID pairs of our own

The AE halves were read on 1.6.1170, the SE halves on 1.5.97 (2026-09-18), all with `tools/disasm.py`. Across the line the IDs do not correspond, so an SE half is found by shape (`--match`: the AE body with every address made the same, looked for among every function of the SE build) and settled by what reaches it (`--refs`: the same callers at the same offsets inside them), or by the same route that found the AE half.

| What | IDs (SE, AE) | Used by | How the SE half was found |
|---|---|---|---|
| Turn an actor toward a point, through its movement controller (the UseWeapon procedure's own call out of combat) | 36818, 37834 | `TurnToward`, `src/game/Packages.cpp` | the one function of that shape on 1.5.97; the same eight callers as on AE, three of them at the same offset inside the caller |
| Take that point back | 36823, 37839 | `StopTurning`, same file | seventeen SE functions share the six-instruction shape; 36823 alone has AE's twenty-five callers, the UseWeapon update among them at the offset where AE calls |
| The global map from a quest alias to its `BGSOverridePackCollection`: its capacity field, 0x0C into a `BSTScatterTable` (the sentinel is at 0x18, the entries pointer at 0x28; the loader is 24013 on AE) | 502247, 369298 | `CheckAliasOverrideLists` and `OverrideListsOf`, same file | AE inlines the table lookup into four readers (12722 to 12725); SE keeps it as a function (12793), reached from the readers' twin (24166, the exact shape of AE's 24670), and that function reads capacity, sentinel and entries at 0x1dd3e64, 0x1dd3e70 and 0x1dd3e80: the same spacing as AE's 369298, 369300 and 369301 |
| `ActorEquipManager::UnequipSpell(actor, spell, source)`, what Papyrus's `Actor.UnequipSpell` tail-calls after its null check: source 0 the left hand, 1 the right, 2 the voice, turned into the matching equip slot inside | 37947, 38903 | `UnequipSpellNow`, `src/game/Pins.cpp` | the route that found the AE half: the `"UnequipSpell"` string, its one reference in the Papyrus registration (53960), the native registered beside it (0x94a910), and its tail jump; the body is instruction for instruction AE's, dispatching the source to 23150, 23151 and 23153 then calling the slot overload 37946 |
| `ActorEquipManager::UnequipShout(actor, shout)`, the same for `Actor.UnequipShout` | 37948, 38904 | `UnequipShoutNow`, same file | the same route; the native at 0x94a870 |

An ID names the same thing in every build of its line only if that library's authors matched it. On a build whose library lacks one, CommonLib stops the game at the first use with "Failed to find the id within the address library" (`src/REL/IDDB.cpp`), rather than skipping. That matching was checked on 2026-09-18 for every ID on this page, by loading the databases in `AddressLibrary/SKSE/Plugins/` directly (`python tools/addrlib.py --all <ids>`): each AE half is present in all thirteen AE-line databases, 1.6.317 through 1.6.1179 and the two 1.7 ones, at a different offset in each build, which is the library doing the job it exists for; 1.7's database is a different file format, format 5, and the AE numbering carries on into it. The SE halves are present in the ten SE databases the same way. Not yet run on SE: the plugin builds for it, and the rows above are read from its executable, not from play.

## ID pairs CommonLib carries

| What | IDs (SE, AE) | Used by | Checked |
|---|---|---|---|
| `TESPackage::CreatePackage` | 28732, 29496 | `src/game/Forms.cpp` | AE read against 1.6.1170 (`dev/MAGIC.md` "Forms at runtime"); the same body in 1.7.104; the SE ID is the library's, and its body on 1.5.97 is shorter (111 instructions to AE's 123) and not read |
| `ActorEquipManager::EquipObject`, `EquipSpell`, `EquipShout`, detoured | 37938 / 38894, 37939 / 38895, 37941 / 38897 | `src/game/Pins.cpp` | CommonLib's pairs; run on 1.6.1170 only; the same bodies in 1.7.104. On 1.5.97, `EquipSpell` and `EquipShout` have instruction for instruction the AE shape; `EquipObject` is 74 instructions to AE's 117, so AE added to it, and what was added is not read |

Across the line the comparison is of shape only, every address made the same, since the IDs do not correspond. `Actor::DeselectSpell` (37820 / 38769), read the same way, differs in one thing: an Actor field at 0x1C0 on SE and 0x1C8 on AE, the layout difference CommonLib carries for the class.

## Vtable slots

| What | Slot | Used by | Checked |
|---|---|---|---|
| `CombatInventoryItem`'s score call, replaced in each entry class's vtable | 0x0C | `kCalculateScoreSlot`, `src/game/Pins.cpp` | the vtables come from CommonLib for each runtime; the slot was read on 1.6.1170's melee entry (`dev/UNIQUE.md`); all twenty-one hooked tables have the same ID in every one of their first sixteen slots on 1.7.104 as on 1.6.1170 (2026-09-18). On 1.5.97 the slot holds the same function shape in the ranged, block, staff and every magic table (SE 43863 for all the casters, as AE 45082 is), and a different one in the melee, shield and torch tables, with the slots around it matching, so the slot is the score there too and those three score differently |

## Layouts CommonLib does not map

Each is found or checked at load against vanilla records, and what does not read as expected turns its feature off rather than being written through. Another build laying one out otherwise costs the feature, not the game.

| What | Found by | Off when it fails |
|---|---|---|
| A package's Spell, Target and CastTime inputs (`BGSPackageDataTargetSelector` and the float data) | `Calibrate`, `src/game/Packages.cpp` | every cast, shout and power rule |
| A package's Int, Bool and Location inputs, and a named form's target type | `CalibrateWeapon`, same file | Power Attack by package (the animation event stays) |
| The alias override map's table: CommonLib's `BSTScatterTable` layout, copied because the global itself is not mapped | `CheckAliasOverrideLists`, same file | the lists are not read |

## Engine behaviour read from 1.6.1170's code

Not addresses: copied into the plugin as logic, and wrong on another build only if Bethesda changed it there. Whether it did is readable without running anything: disassemble the function in both builds, replace every address with the ID it resolves to, and compare. Done for every function below and above on 1.6.1170 against 1.7.104 (2026-09-18): the same instructions in the same order in all of them. Three (26429, 49170, 24013) differ only in the raw displacement of one `[rip + ...]` data reference, which moves with the build, and 29496 only in the padding after its last `jmp`.

- The stamina a power attack and a bash cost (26429; `dev/ACTIONS.md` 6).
- How far a swing has to reach (47273 and 47276; `ReachDistance` and `BodyRadius`, `src/game/Sensors.cpp`): centre to centre, flat from a height difference of 48, less both bodies' radii, each the bound max Y times the scale or 16.
- What the UseWeapon procedure does: its facing check, its turning out of combat and not in one, and how its inputs map (`dev/ATTACK.md`).
- The combat animation actions a block is raised and lowered with (3 and 4), and the idle tree's gates on a bash (`dev/ATTACK.md` "How the engine bashes").
- A power bash as the combat AI's melee chooser makes one (49170): a right attack `CombatAnimation` with the attack's event in its output (`PerformRightAttackWith`, `src/game/Blows.cpp`). CommonLib maps the constructor and `Execute` for both runtimes; the shape was read from 1.6.1170.
- Which word of power the player has unlocked: bit 16 of the word's own form flags. `PlayerCharacter::UnlockWord` (vtable 0xD0) tail-calls the flag setter (14640), which sets that bit at `form + 0x10` and notifies through vtable slot 0x50; `TESShout::GetKnown` (vtable 0x17) reads the same bit off the first variation that has a word, and answers false for a shout with no word at all. Read out of the running process with `tools/livedisasm.py`. Used by `WordUnlocked` and `HighestUnlockedWord`, `src/game/Magic.cpp`: a shout with no word unlocked is greyed on the Magic tab, refuses its voice cell and is offered to no rule, and a shout rule trims the record to the unlocked words for the lease (`RequestShout`, `src/game/Packages.cpp`). CommonLib names no accessor for it on a word: `GetRandomAnim` is that bit on other form types.

## Builds on disk

`C:\Modding\SkyrimVersions\<version>\SkyrimSE.exe` is that build's executable unpacked with Steamless (`C:\Modding\Steamless\Steamless.CLI.exe <exe>` writes `<exe>.unpacked.exe` beside it), with the Steam original kept as `SkyrimSE.steamstub.exe`. `tools/disasm.py --version <version>` reads from there, and pairs it with the database of that version under `AddressLibrary/SKSE/Plugins/`, which holds the v13 all-in-one: every Steam build from 1.5.3 to 1.7.104 plus GOG's 1.6.659 and 1.6.1179. Two files there say 1.6.1170.0 inside: `versionlib-1-6-1170-0.bin` is the one whose offsets read as code in the Steam exe; `versionlib-1-6-1170-0-1.bin` is another table (which build it is for is not known), and reading the Steam exe through it gives garbage. The tools pick by file name.

| Build | On disk | How it was got | SKSE |
|---|---|---|---|
| 1.6.1170 | yes | the installed game, via `tools/Downgrade-Skyrim.ps1` (`dev/DOWNGRADE.md`) | 2.2.8 |
| 1.7.104 | yes (2026-09-18) | the pre-downgrade backup the same script keeps beside the game folder; Steamless reports its code section is not encrypted, so the Steam file would have read as well | 2.3.1 |
| 1.5.97 | yes (2026-09-18) | `download_depot 489830 489833 2289561010626853674` in the Steam console (`steam://nav/console`): the exe depot at its last Special Edition manifest, one file, landing in `steamapps\content\app_489830\depot_489833\`. Special Edition and Anniversary Edition are the same Steam app; "AE" is the 1.6+ executable plus the Anniversary Upgrade's content, so owning it is owning every manifest. Running 1.5.97 would also need that build's `Skyrim.esm` and `Update.esm`, from depots 489831 and 489832 at their 1.5.97 manifests, which the Nexus manifest list (article 6536) has. | 2.0.20 |
| 1.7.99 | no | Steam, 2026-08-20 to 08-27 (the Address Library's file dates); its manifest is on neither list read | not looked up |
| 1.6.1179 | no | GOG only, never a Steam manifest | 2.2.6 |

1.7.104 is the current Steam build (2026-08-27); its exe depot manifest is 4886117324142477814, from the appmanifest the downgrade left read-only. That 1.7 "moved every function but changed no data structure" is what was said around the update, not read from a source here; the comparisons above are what was measured of it. The submodule CommonLib reads the 1.7 database format already (`src/REL/IDDB.cpp`, format 5) and classes any 1.6+ build as AE.

## Testing against another build

Two layers, and only the first exists.

**Static, no game.** What can be answered from an exe and its database: is each ID in the library (`tools/addrlib.py --all`), does each hooked vtable have the same slot sequence (`tools/disasm.py --version <v> --vtable <id>`), and does each function whose logic was copied still have the same body (the normalised comparison above, a one-off script this time). Those three questions, asked of the IDs on this page for every build on disk, are one script away from being a check that runs like the tests do; the list of IDs is this page's, and lives here until that script holds it.

**Dynamic, the game running.** What the static layer cannot answer: the layouts CommonLib does not map (the `Calibrate` probes, which read vanilla records at load), the detours actually placing, and the plugin doing its job. Each build needs its own game folder (the exe with that build's `Data`, run through that build's `skse64_loader.exe`, as the Nordic Souls "Stock Game" folder is run outside Steam's), the matching SKSE, the all-in-one Address Library, an SKSEMenuFramework build for it (Nexus 120352; 3.18, 2026-09-17), and an MO2 instance pointed at it. The log says at load which probes passed. For 1.7.104 that is a second game folder and nothing else. For 1.5.97 it is also a plugin built with `ENABLE_SKYRIM_SE` back on (CommonLib refuses to load an AE-only DLL on SE) and the five AE-only IDs above resolved first, or their features off there.

## Not version-bound

Vanilla form IDs (Mercer's, Colette's, Tsun's, Edorfin's, the Sovngarde heroes' and Vilkas's packages; `DialogueFollower` and its combat override list; Fast Healing; the hand slots; the Block tree's Power Bash perk, 058F67) are the same in every build of `Skyrim.esm`. The school's Dual Casting perks are not named by id at all: the engine's own `CanDualCastSpell` entry point answers for the spell, so a mod's perk counts too. Game settings are read by name. A load order that edits or removes one of these fails the matching load-time check and turns its feature off.
