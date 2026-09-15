# Version-bound dependencies

What the plugin takes from one build of the game rather than from CommonLib or the game's data: what each is, what uses it, what happens on another runtime, and how far it was checked. The plugin is built with CommonLib's SE, AE and VR support all on, and every entry here was read on **1.6.1170** alone. Each is to be resolved before shipping: its Special Edition (and VR) counterpart read and checked, or the feature it serves declared AE-only (`docs/TODO.md` "Toolchain").

A new dependency of this kind gets a row here when it is added.

## Address Library IDs with no Special Edition ID

Called only when `REL::Module::IsAE()`; on SE and VR the feature is skipped.

| What | AE ID | Used by | Off AE |
|---|---|---|---|
| Turn an actor toward a point, through its movement controller (the UseWeapon procedure's own call out of combat) | 37834 | `TurnToward`, `src/game/Packages.cpp` | not turned; a power attack needs the follower already facing the target |
| Take that point back | 37839 | `StopTurning`, same file | nothing to take back |
| The global map from a quest alias to its `BGSOverridePackCollection`: the capacity field the alias loader (24013) reads, 0x0C into the table | 369298 | `CheckAliasOverrideLists` and `OverrideListsOf`, same file | not read; a power attack's record goes on an alias's package array with IgnoreCombat and is turned toward the target |

An AE ID names the same thing in every AE build's Address Library only if that library's authors matched it; these were read on 1.6.1170. On an AE build whose library lacks one, CommonLib stops the game at the first use with "Failed to find the id within the address library" (`src/REL/IDDB.cpp`), rather than skipping.

To resolve: find each in the SE executable (`tools/livedisasm.py` against a running SE, or `tools/disasm.py` against an unpacked one), by what calls it and what it calls, and make it a `RELOCATION_ID(se, ae)`. The alias map could instead go to CommonLib as a mapped global, with both IDs.

## ID pairs whose Special Edition half was never read

| What | IDs (SE, AE) | Used by | Checked |
|---|---|---|---|
| `TESPackage::CreatePackage` | 28732, 29496 | `src/game/Forms.cpp` | AE read against 1.6.1170 (`docs/MAGIC.md` "Forms at runtime"); the SE ID is the library's |
| `ActorEquipManager::EquipObject`, `EquipSpell`, `EquipShout`, detoured | 37938 / 38894, 37939 / 38895, 37941 / 38897 | `src/game/Pins.cpp` | CommonLib's pairs; run on 1.6.1170 only |

## Vtable slots

| What | Slot | Used by | Checked |
|---|---|---|---|
| `CombatInventoryItem`'s score call, replaced in each entry class's vtable | 0x0C | `kCalculateScoreSlot`, `src/game/Pins.cpp` | the vtables come from CommonLib for each runtime; the slot was read on 1.6.1170's melee entry (`docs/UNIQUE.md`) |

## Layouts CommonLib does not map

Each is found or checked at load against vanilla records, and what does not read as expected turns its feature off rather than being written through. Another build laying one out otherwise costs the feature, not the game.

| What | Found by | Off when it fails |
|---|---|---|
| A package's Spell, Target and CastTime inputs (`BGSPackageDataTargetSelector` and the float data) | `Calibrate`, `src/game/Packages.cpp` | every cast, shout and power rule |
| A package's Int, Bool and Location inputs, and a named form's target type | `CalibrateWeapon`, same file | Power Attack by package (the animation event stays) |
| The alias override map's table: CommonLib's `BSTScatterTable` layout, copied because the global itself is not mapped | `CheckAliasOverrideLists`, same file | the lists are not read |

## Engine behaviour read from 1.6.1170's code

Not addresses: copied into the plugin as logic, and wrong on another build only if Bethesda changed it there.

- The stamina a power attack and a bash cost (26429; `docs/ACTIONS.md` 6).
- How far a swing has to reach (47273 and 47276; `ReachDistance` and `BodyRadius`, `src/game/Sensors.cpp`): centre to centre, flat from a height difference of 48, less both bodies' radii, each the bound max Y times the scale or 16.
- What the UseWeapon procedure does: its facing check, its turning out of combat and not in one, and how its inputs map (`docs/ATTACK.md`).
- The combat animation actions a block is raised and lowered with (3 and 4), and the idle tree's gates on a bash (`docs/ATTACK.md` "How the engine bashes").
- A power bash as the combat AI's melee chooser makes one (49170): a right attack `CombatAnimation` with the attack's event in its output (`PerformRightAttackWith`, `src/game/Blows.cpp`). CommonLib maps the constructor and `Execute` for both runtimes; the shape was read from 1.6.1170.

## Not version-bound

Vanilla form IDs (Mercer's, Colette's, Tsun's, Edorfin's, the Sovngarde heroes' and Vilkas's packages; `DialogueFollower` and its combat override list; Fast Healing; the hand slots) are the same in every build of `Skyrim.esm`. Game settings are read by name. A load order that edits or removes one of these fails the matching load-time check and turns its feature off.
