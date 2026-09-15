# Making a follower cast a spell

**Status: works, measured in game (2026-09-02).** A rule fires at 43% health,
the follower's AI picks up our package on the same tick, her own animation
graph reports `Fast Healing -- OURS` 1.4 s later, health goes 75 -> 175, and she
is back to fighting on the next tick. Two consecutive cycles, on **1.6.1170**.

This file is the current design, the facts that were expensive to learn, and
the routes that do not work. It replaces a longer chronological version; the
sequence of runs that shaped the design is in the git history of this file.

---

## Why a potion is easy and a spell is not

**A potion is a state change. A cast is a performance.**

Drinking is an inventory operation. `ActorEquipManager::EquipObject` is the
game's own equip routine, equipping a potion consumes it, and the magic system
applies the effect at once. No animation to schedule, no AI decision.

Casting is something an actor *does over time*: charge, release, magicka drawn
at a particular instant, interruptible by a stagger, aimed at something. All of
that lives in the animation graph and the AI. There is no "perform this" entry
point because performing is not a state you can set. **The spell can be made
to happen; the follower cannot be made to perform it** -- except by giving her
AI a reason to. That is what AI packages are for, and it is the whole design.

---

## How a cast happens

Records say *what* she does; the C++ says *when*. No Papyrus.

### The records (made in memory, one set per follower; no plugin file)

**Verified in play 2026-09-08**, with no plugin file in the load order: Conjure Flame Atronach through a spell slot, Voice of the Emperor and Unrelenting Force (`variation 2`, all three words) through the shout slots, the list back to vanilla after each. The ESP is gone from the repo; its records are in git history before this date.

Each follower gets one of each, made the first time the tick sees them and kept by reference ID until the game quits, so one who rejoins, or turns up in another save, gets theirs back. Each form takes the next free FormID from FF3F0800 up, in the order made: an ID the form map or the loaded save already holds is skipped ("Forms at runtime" below).

| record | contents |
|---|---|
| cast package | a copy of `TG08BMercerCombatOverrideCastAtPlayer` (UseMagic template): Location NearSelf r10000, HoldWhenBlocked off, CastTime 0.5..1, Cooldown 1..1, NumToCast 1..0, DualCast off. After the copy: Spell = Fast Healing (written and read back, the proof the layout holds on a copy), Target = Self, flags **IgnoreCombat** and nothing else. One condition: `GetIsReference(<holder>) == 1` |
| power word | a word of power for the wrapper below; a label, no behaviour |
| power shout | a one-word wrapper shout: word one's spell = Fast Healing (repointed at the rule's power), recovery 1 |
| shout package | a copy of `MQ305TsunReturnShout` (Shout template), Shout = its wrapper, Target = Self, HoldWhenBlocked off, flags IgnoreCombat and WeaponDrawn. Same condition. How a POWER is performed (`docs/ACTIONS.md` 7) |

There is no faction any more. The condition's parameter is a pointer the C++ writes: the holder's actor for the lease, null after. `src/game/Forms.cpp` makes the forms; "Forms at runtime" below is what was established about doing that.

### Where a record goes: the front of her own package stack

An actor runs one package at a time, chosen by a walk over candidates in priority order, first passing condition wins. The candidates that matter come from quest aliases: every alias an actor fills is instanced for them as an array of packages attached to the actor (`ExtraAliasInstanceArray`, one `BGSRefAliasInstanceData` per alias with its `instancedPackages`; the library maps all of it), and the walk goes quest by quest through those arrays, highest quest priority first. The array holding the package running her now is therefore the one at the top of her walk.

When a cast rule fires, `PutOnStack` puts the leased record at the **front** of that array -- the fullest array when nothing of hers is running -- and the AI is re-evaluated at once. The lease's condition, `GetIsReference(<holder>)`, gates it: the engine's implementation is a null-safe pointer compare between the evaluating actor and the parameter (read from the executable), so pointing the parameter at her is one write, nothing is written to her, and the record passes for her alone. On release the parameter is cleared and her arrays are walked to take the record out; only a flag is kept across the lease, since the arrays are hers and go with her. In memory only, per actor, nothing shared: between casts her arrays are exactly what her quests gave her, and two followers casting at once each hold their own record on their own stack.

This reaches every follower the same way, whatever drives them: one in the vanilla `DialogueFollower` alias, Serana on Dawnguard's `DLC1NPCMentalModel`, a follower on their author's own quest or a framework's. Verified in play 2026-09-09 (Nordic Souls): Megara, on `AK69SugarandSpiceQuest` alias 0, ran the record at once (`current package after evaluate: FF3F0800 (OURS)`) and fired the rule's dual-cast Healing Hands from it, lease after lease; the vanilla-alias followers likewise.

The game's own example of a package cast mid-fight is Mercer Frey in *Blindsighted*: a UseMagic package gated on quest stage makes him cast Nightingale Strife at the player. Ours is the same package with `GetIsReference` as the trigger, on the actor's stack.

The actor's package extra data is part of the save (`Actor::ChangeFlags::kPackageExtraData`), and the engine does save a runtime package an actor is running ("Forms at runtime" below). No save is made inside a lease: every lease is released on the save message, before the engine writes, and the release re-evaluates the follower's package at once.

### The C++ (`src/game/Packages.cpp`)

1. **A cast rule fires.** Take the follower's own record, put it at the
   front of their package stack (above). Repoint its Spell
   input at the rule's spell and its Target input by the spell's **delivery**:
   a Self-delivery spell casts on her, anything else goes at the enemy she is
   engaging (a non-hostile targeted spell such as Healing Hands will need the
   player instead; not authored yet). Both inputs are found by canary, see
   below. Create a
   `SlotLease`, whose constructor points the slot's condition at her. Ask the
   AI to re-evaluate.
2. **The AI casts.** Our package now passes. `IgnoreCombat` takes her hands
   away from her combat AI; the UseMagic procedure interrupts what she was
   doing, charges, fires. Animation, cost and interruption are the game's.
3. **Release.** The tick destroys the lease on the first of: her animation
   graph emitting a spell-fire event **for our spell** (read from the spell
   equipped in the firing hand -- her own firebolts are logged and ignored);
   the AI having dropped the package; or a four-second deadline. The
   destructor clears the condition and re-evaluates, so she returns to
   fighting at once. The record waits for their next cast.

   A **concentration** spell (Flames, vanilla Healing) is a stream, and its
   fire event marks the *start*. Measured: releasing on it cut Flames off at
   0.35-0.65 s, whichever tick landed first. So for a concentration spell the
   fire event is not a release signal; the record's two `CastTime` floats are
   set to the sustain time (the rule's numeric argument, default 3 s) and the
   deadline is extended by it. The stream then runs for the time the record
   says, and the lease ends when the target dies, the AI drops the package, or
   at the deadline.

**One set of records per follower.** Every input in a record -- spell, target, cast time -- belongs to one follower, so nothing in it can be shared by accident. While a follower holds one of theirs, their cast rules report *busy* for that turn, spend no cooldown, and the next rule gets its turn.

Every lease is released on SKSE's save message, which arrives before the engine writes the file: a save never holds a follower running one of our packages, carrying a wrapper in their shout list or their voice slot, or shouting a re-typed power. A game load drops every lease; the records stay.

### Forms at runtime

What `src/game/Forms.cpp` relies on, read from the **1.6.1170** executable rather than from headers or memory. `tools/disasm.py` prints any function by Address Library ID; it needs `pip install capstone pefile` and a copy of `SkyrimSE.exe` unpacked with [Steamless](https://github.com/atom0s/Steamless) (`SKYRIM_EXE=<that copy>`), because the Steam exe's code section is SteamStub-encrypted and disassembles to noise as installed. The 1.6.1170 IDs named below are the AE ones from `versionlib-1-6-1170-0.bin`.

- **A form constructed outside file loading gets a dynamic FormID and is registered.** `TESForm::TESForm` (14593): when the data handler exists and is not loading files, the ID comes from its next-ID routine (13740), which hands out FF000800..FF3FFFFF, skipping IDs already in use, and wraps. The save-game loader (448563) restores that counter from the save. So a plugin's runtime forms and a save's created objects (potions, enchantments) compete for the same IDs, and what `SetFormID` (14666: erase old, set, insert) does on a collision is not visible from the code. Read from the running 1.6.1170 process (2026-09-14): the allocator skips an ID for which `TESForm::LookupByID` finds a form or `BGSSaveLoadGame::IsFormIDInUse` (35593) says the loaded save holds it, and wraps to FF000800 when the counter's low 24 bits reach 3FFFFF. So FF3F0800 is within its reach, and within a 22-bit created-object reference; what keeps a save's objects off ours is distance, since a save's counter gets there only after some four million created forms, and the allocator skips ours once they are registered. Ours take the **next free ID from FF3F0800**, checked the same two ways; one another plugin already holds is skipped.
- **`TESPackage::CreatePackage(type)`** (29496; alandtse's fork carries it as `RELOCATION_ID(28732, 29496)`) allocates a package and calls `SetPackType` (29525), which for type 18 (an instance of a template) allocates a `TESCustomPackageData`. The constructor already does that for a fresh package. `procedureType` (+D8) is set to -1 by the constructor and loaded from the save by `LoadGame`; nothing at file load writes it, so it is runtime state and is left alone.
- **`TESForm::Copy` and `CreateDuplicateForm` copy nothing package-specific**: `TESPackage`'s vtable has the base `TESForm` entries in both slots. The engine's copy of package data is **`TESCustomPackageData::Copy`** (29657): the template link, then the input list through `BGSPackageDataList`'s copy (27428), which for each input looks its type name up in a registry, creates a new one and calls `Assign` on it -- a target input's `Assign` (27598) allocates its own `PackageTarget` -- then clones the procedure tree and, when the template link is set, shares the source's name map. The two tree hooks it calls forward to the shared UseMagic procedure object, which every package on the template already shares.
- **`TESCustomPackageData::InitItem`** (29658) resolves the template link from a FormID, shares the template's tree and name map, and creates **no inputs**. A file-loaded instance's inputs come from the file. So a runtime package copies its inputs from a finished vanilla instance (Mercer's; Tsun's for the Shout template), not from the template record, whose defaults differ anyway (radius 500, HoldWhenBlocked on, CastTime 2..3, Spell as an object *type*).
- **`GetIsReference`'s condition function** (at 0x32DB20 in that build; found through the script-command table entry, which is what the engine's condition evaluator dispatches through): result 0; if the parameter is non-null and its form type is a reference type (0x3D..0x46) and equals the evaluating reference, result 1. Null-safe, so a slot's "off" state is a null parameter. `GetFactionRank`'s (0x32DBB0) likewise takes a faction *pointer*: condition parameters are resolved to pointers at load.
- **The engine saves a runtime package an actor is running.** The actor loader (38966, an `Actor::LoadGame` helper) creates a package of the saved type with `CreatePackage` and fills it from the buffer when the saved ID is a created one -- for a custom package, a hollow one with no inputs and no conditions. SKSE's `kSaveGame` message is dispatched from its `SaveGame_Hook` **before** the original save routine runs, so every lease is released there.
- **The voice slot is saved as a bare form ID.** Read from the running 1.6.1170 process (2026-09-14). `Actor::SaveGame` (37649; `Character`'s slot jumps to it) writes the four `selectedSpells` and then `selectedPower` (+1E8 on AE) through 36048, which takes the form's ID and writes a 3-byte reference (35928): a Skyrim.esm ID as `400000 | id`, a created one as `800000 | id`, the rest through the save's plugin table. `Actor::LoadGame` (37650) decodes it (36000, 35927: a created reference comes back as `FF000000` with its low 22 bits), looks the ID up (14617, which Engine Fixes hooks) and stores the result: a hand slot through `dynamic_cast` to a spell (109689), the voice slot with no check at all. `Actor::FinishLoadGame` (37652) then prepares what the voice slot holds if it is a spell (type 16) or a shout (77: each variation's spell, through 23367), and leaves anything else where it is. The Shout procedure readies what it fires, so a power's lease left its wrapper in the slot after the wrapper came out of the shout list, and a save wrote `FF3F0802` as `BF 08 02`. Loaded after a restart, that finds nothing, since our forms are made after the load; loaded without quitting, it finds a live form of ours, perhaps another follower's (IDs go out in the order the tick first sees followers) and, after a skipped ID, perhaps not a shout. So `TakeWrapper` clears the voice slot of any form `Forms.cpp` made (`MadeByUs`), when a lease is released, and so on the save message, and in the tick's sweep. By a plain write: the panel's unequip is Papyrus's `UnequipShout`, which the VM runs a frame later, after the save is written. A shout rule leaves the real shout readied, as the AI's own shouts do, and a hand holds the spell itself: neither is ours.
- **PKDT, byte for byte, of the proven ESP records**: flags `00100000` (IgnoreCombat), type 18, interrupt override **0** ("None" in the file; the header's enum names 0 `kSpectator` and -1 `kNone`, so do not go by the names), speed 2 (Run), interrupt flags 0. Mercer's record differs in interrupt override (Combat) and combat style; the copies are set to the proven values.

### Clocks

Cooldowns and lease deadlines run on **game time** converted to real seconds
at the timescale (`TacticsSeconds()`, read from the hour-of-day global, which
keeps sub-second precision where "hours passed" does not). It stops in menus
and jumps on wait, sleep and fast travel -- the same behaviour as the engine's
own per-actor countdowns such as shout recovery.

### The log

Each fired cast rule produces, in order:

```
alias: quest 000750BA "DialogueFollower" alias 0 "Follower"  <- follower alias
000B9986 holds the condition (leased)
current package after evaluate: FF3F0800 (OURS)
anim 000B9986: right hand fired 000C969B "Firebolt" -- her own, ignored
anim 000B9986: left hand fired 0007231C "Fast Healing" -- OURS
packages: Marcurio releases FF3F0800 after 2.0 s: spell fired
000B9986 releases the condition (lease ended)
```

| if instead | it means |
|---|---|
| alias: NO, or "in NO quest alias at all" | Recruited from the console. Only `SetFollower` fills the alias: use "Follow me", or `cqf DialogueFollower SetFollower <refid>` (unverified). |
| not ours yet -- watching, then deadline | Condition set, package never selected. Check the list order line at load, and that the follower's `forms:` lines all read back. |
| OURS, then deadline with only her own spells firing | The package never got her hands. `IgnoreCombat` is off the records. |
| OURS, then deadline with nothing firing | She was staggered or otherwise stuck through the window. |

### Test procedure

0. The load's `probe:` lines come before any follower exists. A follower's
   `forms:` lines (one per form, "born FF00xxxx", none saying NOT
   registered) and the `packages:` line naming their cast package, shout
   package and wrapper come the first time the tick sees them.
1. `bat ftspawn`, click her, `bat ftmake` (grants Oakflesh, potions, relationship
   rank -- not teammate status).
2. **Talk to her, "Follow me."** The alias line in the log is the check.
3. A self-cast rule: `IF self health < 50% THEN cast Fast Healing ON self`.
4. `bat ftbear`, then read `FollowerTactics.log`.

Edits under `bat/` do nothing until `tools\deploy-tests.ps1` runs: the game
reads the copies in the Skyrim root.

---

## Facts worth not rediscovering

Each of these cost at least one test round.

- **The AI takes 0.5 to 1.0 s to begin a cast or shout it has already
  selected**, occasionally 3 s. Measured 2026-09-08 with the weapon state
  logged at arming, pick-up and begin: "drawn" throughout, so it is not a
  sheathe-and-draw around the package (the Weapon Drawn flag on the shout
  slots was the test of that). It is the procedure's own start-up, and the
  same for a hand cast. The arm window covers it.
- **The spell-fire animation event fires for every spell she casts.** A
  Destruction mage emits `MRh_SpellFire_Event` constantly. Releasing on the
  first one cancels our package before it casts. Check the spell.
- **`MagicCaster::currentSpell` is already null at the fire event.** The spell
  still equipped in that hand (`selectedSpells[kLeftHand/kRightHand]`) is not,
  and the UseMagic procedure equips what it casts.
- **`IgnoreCombat` is required.** Without it her combat AI keeps her hands and
  the package never fires. It was removed once because she stood idle after a
  heal; the idling was the missing re-evaluate on release, not the flag.
- **The UseMagic package does not complete on its own** after NumToCast=1
  casts. Release must come from us. (NFF solves this at the record level: its
  heal template runs UseMagic and a 3 s Wait side by side, so the package ends
  by timeout. An option if the C++ deadline ever needs to move into content.)
- **`AddToFaction(faction, -1)` removes her from the faction**, and
  `GetFactionRank` then reports -2. (From the faction-rank era; kept because
  it is not what the header says.)
- **A console teammate is not an alias follower.** `setplayerteammate` and
  `addtofaction CurrentFollowerFaction` satisfy every check except the one
  that matters. Read the actor's `ExtraAliasInstanceArray`, not a quest call.
- **Packages have no editor ID at runtime.** Log FormIDs, or the list prints
  as `[]`.
- **Calendar "hours passed" is a float**: sub-second precision is gone after
  a few hundred game days. Hour-of-day stays precise; count midnight yourself.
- **Two spells can share a display name.** Marcurio's heal is `0007231C`; the
  vanilla one is `0002F3B8`. Both are "Fast Healing". Compare FormIDs.
- **Where a package's inputs live**, found by canary rather than guessed. An
  input's payload is at `IPackageData + 0x10`, whatever its kind: a float
  input holds the float there (CastTime read 0.5 and 1.0 at +10, measured;
  the +08 slot the headers suggested read 0 / 0), and the Spell and Target
  inputs hold a pointer there to a `PackageTarget` (mapped by CommonLibSSE:
  type at 00, form-or-handle union at 08). Self read as type **6** in the engine, not
  the 5 the record library's ordering implies -- which is why the type values
  are read from authored records, not assumed: Colette's practice heal
  (`WCollegeColettePracticeHeal13x2`, 098BAD: Fast Healing on Self) for Self,
  Mercer's for a specific reference. The template's name map spells it
  `SPELL`; compare case-insensitively. The map lives on the template, not on
  the copy.
- **The sensor must report whom she is fighting.** `currentCombatTarget` on
  the actor's runtime data. Without it, "cast at current target" resolves to
  no target and the rule never fires.
- **Copying a vanilla package copies its inputs.** Ours came from
  `MG07AncanoCastAtEye` and shipped aiming at the Eye of Magnus with a
  ten-million-second cast time. Read every input of a copied record.
- **`BGSRefAlias::ForceRefTo`** is what Simple Follower Framework uses to
  fill its own aliases from C++; it is in the library we build on.

---

## What does not work, and why

Seven mechanisms were tried before the package route. Recorded so nobody
repeats them.

1. **`CastSpellImmediate` / `Spell.Cast`.** Applies the effect, never animates
   an actor. The CK wiki says so, and DynamicAnimationCasting uses exactly this
   call *because* the animation is already playing.
2. **Sending animation events.** `MRh_SpellFire_Event` is something the graph
   *emits*. NPC Spell Variance receives it; it never initiates a cast.
3. **`SetCurrentSpellImpl` + `RequestCastImpl`.** Internal virtuals the game's
   update loop calls. Never demonstrated from outside.
4. **`PutCreatedPackage` + `EvaluatePackage`.** The engine's created
   package, what Papyrus uses to walk an actor somewhere: placed and
   re-evaluated, her running package stays current in a fight (Nordic Souls,
   2026-09-09). The alias arrays are what combat reads.
5. **Swapping `CombatMagicCaster::magicItem`.** The write takes and changes
   nothing useful; the caster in question had selected a potion.
6. **Boosting `CalculateScore`.** The spell already out-scored every
   alternative 4.5x. Score was never the problem.
7. **Hooking `CombatMagicCasterRestore::CheckStartCast`.** We can answer the
   AI's question; we cannot make it ask. The health caster never evaluates
   above a very low threshold. The hook code was removed once the package
   route shipped; it is in the history before commit `28ddd3b`
   (`src/game/CombatHook.cpp`).

Also rejected: **changing the restore-health threshold game setting.** It
would change when every NPC in Skyrim heals.

| approach | reaches |
|---|---|
| equip spell, instant apply | one follower, one action |
| override-list splice + condition lease (shipped) | our records (made at load), the follower alias, our followers |
| a QUST at priority 99 | would not work: combat ignores the package stack |
| combat AI hooks | every actor in the game |
| GMST threshold | every actor, permanently |

---

## Follower frameworks, as reference

Read from their plugins with houseCARL, for where each keeps its followers; the stack route reaches them all alike, since every one fills an alias whose packages are instanced on the actor.

| framework | followers live in |
|---|---|
| vanilla | `DialogueFollower` alias 0 |
| Dawnguard (Serana) | `DLC1NPCMentalModel` alias 0 |
| Simple Follower Framework 2.0.3 | follower 1 in the vanilla alias; 2..8 in `SFF_FollowerQuest` aliases 1..7, filled from C++ with `ForceRefTo` |
| Nether's Follower Framework 2.8.6b | `nwsFollowerPack` `PackAlias1..12` and tiers |
| a custom follower (Megara, Remiel) | the author's own follow quest |

NFF also has `nwsFollowerHealSelf` / `HealPlayer`: UseMagic packages gated on `GetFactionRank` of an NFF faction, the same idiom as ours.

---

## Open

Tracked in `docs/TODO.md`. The casting items there: sustain length in the
editor, the AI occasionally not starting a cast, non-hostile targeted spells,
a target picker, our own quest and aliases.

## What the combat AI will cast (2026-09-03)

Read off the AI's own list of options, the `CombatInventory` on her combat
controller, logged at the start of a fight (the probe in `Tactics.cpp`):

- It is built when the fight begins, from her spell lists and inventory as
  they are at that moment. Removing a spell from her lists after that does
  nothing for the fight in progress: Firebolt was set aside three
  milliseconds after "entered combat" and cast anyway.
- **It skips any spell whose level is above her skill.** Marcurio at
  Destruction 39 and Restoration 45: Chain Lightning and Close Wounds
  (Adept, 50) were left out with magicka at 210/210; every Apprentice and
  Novice spell was in. This is the AI's CHOICE, not a casting limit: a cast
  rule's UseMagic package makes her cast such a spell regardless, and the
  player casts anything with the magicka for it. A pin on such a spell gives
  the AI nothing to reach for; the Magic tab dims its level and says so.
- **An either-hand spell is listed once per hand.** Flames appears twice in
  the list, once with the left slot and once with the right, and the AI put
  the right-hand entry into a pinned right hand. Each entry is judged by its
  own hand, so the entry for the free hand stays available.
- The seven arrays, from what appeared in them: [0] offence (attack spells,
  bows, blades), [1] restoration (healing spells and potions), [3] defence
  (wards, shields), [4] armour spells; [2], [5], [6] empty for a mage.
- With every usable right-hand spell set aside, he drew a dagger. The list
  is the list: the AI falls back to what is left, weapons included.

### How a pin is kept from the AI (2026-09-03): its own scoring

The combat AI chooses from a list of scored options, its own per-follower
combat inventory, and it asks every entry for its score, through a virtual
call, each time it decides what to hold. That call is ours: the scoring
slot in each entry class's table is replaced at load (six weapon kinds,
fifteen kinds of spell entry, one per caster type; any class first seen in
a list at combat start is taken over then). An entry the pins keep from the
AI -- one whose hand a pin holds, unless it is that pin -- scores zero and
is never chosen; everything else gets the class's own answer. Nothing runs
on the tick for it, and it holds however often the AI re-lists its options,
which it does every few seconds for the range it is at. Verified: a pinned
longbow held at melee range against a sword the AI kept re-listing.

The list is expanded per hand: an either-hand spell appears twice, once
with the left slot and once with the right, so with Flames pinned left and
Firebolt pinned right the AI is left with exactly those two entries. A spell
above skill is never in the list and cannot be pinned. Her records are never
touched. Two earlier ways were tried and dropped: removing competing spells
from her record for the life of a pin (it worked, and left her without them
for every menu, script and mod meanwhile), and erasing entries from the list
on each tick (a race the AI won: it re-listed the sword and drew it in the
half second before the next erase, and the update-timer setting was not the
cause).
