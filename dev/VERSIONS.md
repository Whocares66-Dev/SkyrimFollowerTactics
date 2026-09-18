# Version-bound dependencies

What the plugin takes from one build of the game rather than from CommonLib or the game's data: what each is, what uses it, what happens on another runtime, and how far it was checked. Every entry here was read on **1.6.1170**.

**The plugin is built for the AE line alone** (2026-09-17): `ENABLE_SKYRIM_SE` and `ENABLE_SKYRIM_VR` are forced off in `CMakeLists.txt`, which is the resolution this page used to offer as an alternative -- "the feature it serves declared AE-only". An Address Library ID is stable within a line but not across one, so on SE the same number names another function; carrying a second path for a runtime nobody here can test was worse than not building for it. Supporting SE or VR again means reading each ID's Special Edition half first, and the rows below are that list.

A new dependency of this kind gets a row here when it is added.

## Address Library IDs with no Special Edition ID

Simply called: the plugin is AE-only, so `REL::Module::IsAE()` is a compile-time truth and the guards some of these still carry fold away. The "Off AE" column records what would be lost if SE or VR support came back before that ID's Special Edition half was read.

| What | AE ID | Used by | Off AE |
|---|---|---|---|
| Turn an actor toward a point, through its movement controller (the UseWeapon procedure's own call out of combat) | 37834 | `TurnToward`, `src/game/Packages.cpp` | not turned; a power attack needs the follower already facing the target |
| Take that point back | 37839 | `StopTurning`, same file | nothing to take back |
| The global map from a quest alias to its `BGSOverridePackCollection`: the capacity field the alias loader (24013) reads, 0x0C into the table | 369298 | `CheckAliasOverrideLists` and `OverrideListsOf`, same file | not read; a power attack's record goes on an alias's package array with IgnoreCombat and is turned toward the target |
| `ActorEquipManager::UnequipSpell(actor, spell, source)`, what Papyrus's `Actor.UnequipSpell` tail-calls after its null check: source 0 the left hand, 1 the right, 2 the voice, turned into the matching equip slot inside | 38903 | `UnequipSpellNow`, `src/game/Pins.cpp` | nothing takes a spell out of a hand at all: the Papyrus dispatch that used to do it went with the AE-only build, and would have to come back with the runtime |
| `ActorEquipManager::UnequipShout(actor, shout)`, the same for `Actor.UnequipShout` | 38904 | `UnequipShoutNow`, same file | the same Papyrus fallback |

An AE ID names the same thing in every AE build's Address Library only if that library's authors matched it; these were read on 1.6.1170. On an AE build whose library lacks one, CommonLib stops the game at the first use with "Failed to find the id within the address library" (`src/REL/IDDB.cpp`), rather than skipping.

That matching was checked for 38903 and 38904 on 2026-09-17, by loading the databases in `AddressLibrary/SKSE/Plugins/` directly rather than trusting it: both IDs are present in 1.6.317, 353, 629, 1130, 1170 and 1179, at a different offset in each build, which is the library doing the job it exists for. So these two are not pinned to the runtime they were read on -- they hold across the AE line. What they do not cross is the line to SE: that database is a separate ID space, numbered independently, which is why CommonLib spells every such function as a `RELOCATION_ID(se, ae)` pair of two different numbers. It is a separate file format too, and so is 1.7's: `version-1-5-97-0.bin` reads as format 1 and the 1.7 databases as format 5, where `tools/disasm.py` (format 2) cannot follow, so neither was checked here.

To resolve: find each in the SE executable (`tools/livedisasm.py` against a running SE, or `tools/disasm.py` against an unpacked one), by what calls it and what it calls, and make it a `RELOCATION_ID(se, ae)`. The alias map could instead go to CommonLib as a mapped global, with both IDs.

## ID pairs whose Special Edition half was never read

| What | IDs (SE, AE) | Used by | Checked |
|---|---|---|---|
| `TESPackage::CreatePackage` | 28732, 29496 | `src/game/Forms.cpp` | AE read against 1.6.1170 (`dev/MAGIC.md` "Forms at runtime"); the SE ID is the library's |
| `ActorEquipManager::EquipObject`, `EquipSpell`, `EquipShout`, detoured | 37938 / 38894, 37939 / 38895, 37941 / 38897 | `src/game/Pins.cpp` | CommonLib's pairs; run on 1.6.1170 only |

## Vtable slots

| What | Slot | Used by | Checked |
|---|---|---|---|
| `CombatInventoryItem`'s score call, replaced in each entry class's vtable | 0x0C | `kCalculateScoreSlot`, `src/game/Pins.cpp` | the vtables come from CommonLib for each runtime; the slot was read on 1.6.1170's melee entry (`dev/UNIQUE.md`) |

## Layouts CommonLib does not map

Each is found or checked at load against vanilla records, and what does not read as expected turns its feature off rather than being written through. Another build laying one out otherwise costs the feature, not the game.

| What | Found by | Off when it fails |
|---|---|---|
| A package's Spell, Target and CastTime inputs (`BGSPackageDataTargetSelector` and the float data) | `Calibrate`, `src/game/Packages.cpp` | every cast, shout and power rule |
| A package's Int, Bool and Location inputs, and a named form's target type | `CalibrateWeapon`, same file | Power Attack by package (the animation event stays) |
| The alias override map's table: CommonLib's `BSTScatterTable` layout, copied because the global itself is not mapped | `CheckAliasOverrideLists`, same file | the lists are not read |

## Engine behaviour read from 1.6.1170's code

Not addresses: copied into the plugin as logic, and wrong on another build only if Bethesda changed it there.

- The stamina a power attack and a bash cost (26429; `dev/ACTIONS.md` 6).
- How far a swing has to reach (47273 and 47276; `ReachDistance` and `BodyRadius`, `src/game/Sensors.cpp`): centre to centre, flat from a height difference of 48, less both bodies' radii, each the bound max Y times the scale or 16.
- What the UseWeapon procedure does: its facing check, its turning out of combat and not in one, and how its inputs map (`dev/ATTACK.md`).
- The combat animation actions a block is raised and lowered with (3 and 4), and the idle tree's gates on a bash (`dev/ATTACK.md` "How the engine bashes").
- A power bash as the combat AI's melee chooser makes one (49170): a right attack `CombatAnimation` with the attack's event in its output (`PerformRightAttackWith`, `src/game/Blows.cpp`). CommonLib maps the constructor and `Execute` for both runtimes; the shape was read from 1.6.1170.
- Which word of power the player has unlocked: bit 16 of the word's own form flags. `PlayerCharacter::UnlockWord` (vtable 0xD0) tail-calls the flag setter (14640), which sets that bit at `form + 0x10` and notifies through vtable slot 0x50; `TESShout::GetKnown` (vtable 0x17) reads the same bit off the first variation that has a word, and answers false for a shout with no word at all. Read out of the running process with `tools/livedisasm.py`. Used by `WordUnlocked` and `HighestUnlockedWord`, `src/game/Magic.cpp`: a shout with no word unlocked is greyed on the Magic tab, refuses its voice cell and is offered to no rule, and a shout rule trims the record to the unlocked words for the lease (`RequestShout`, `src/game/Packages.cpp`). CommonLib names no accessor for it on a word: `GetRandomAnim` is that bit on other form types.

## Not version-bound

Vanilla form IDs (Mercer's, Colette's, Tsun's, Edorfin's, the Sovngarde heroes' and Vilkas's packages; `DialogueFollower` and its combat override list; Fast Healing; the hand slots; the Block tree's Power Bash perk, 058F67) are the same in every build of `Skyrim.esm`. The school's Dual Casting perks are not named by id at all: the engine's own `CanDualCastSpell` entry point answers for the spell, so a mod's perk counts too. Game settings are read by name. A load order that edits or removes one of these fails the matching load-time check and turns its feature off.
