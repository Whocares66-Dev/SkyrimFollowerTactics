# Making a follower cast a spell

**Status: works, measured in game (2026-09-02).** A rule fires at 43% health,
the follower's AI picks up our package on the same tick, her own animation
graph reports `Fast Healing -- OURS` 1.4 s later, health goes 75 -> 175, and she
is back to fighting on the next tick. Two consecutive cycles, on **1.6.1170**
with CommonLibSSE-NG 3.7.0.

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

### The records (`esp/FollowerTactics.esp`, ESL, master Skyrim.esm)

| record | FormID | contents |
|---|---|---|
| `FT_CastSlot1..8` | 0x800..0x807 | UseMagic template. Spell = Fast Healing and Target = Self (both canaries, both repointed at runtime), Location = NearSelf r10000, CastTime 0.5..1, Cooldown 1..1, NumToCast 1..1, DualCast off, flags **IgnoreCombat**. One condition: `GetFactionRank(FT_CastNow) == slot` |
| `FT_CastNow` | 0x808 | a faction with ranks 0..15, used for nothing but that condition |

Edit them with houseCARL (`housecarl_bulk_apply`, `target=FollowerTactics.esp`,
`in_place=true`), then copy the file into `esp/`. The xEdit script in
`tools/xedit/` is superseded.

### The list they live in

An actor **in combat does not run her package stack**. She runs the **Combat
Override Package List** on her quest alias, top to bottom, first passing
condition wins. The vanilla follower alias (`DialogueFollower` alias 0) has
one: `PlayerFollowerCombatOverridePackageList` (0005C852), two HoldPosition
entries, the last with no conditions.

At load, the C++ inserts our eight packages at the **front** of that list, in
memory only. Nothing vanilla is overridden on disk, nothing is saved, and it is
redone every launch. With no follower in `FT_CastNow`, every one of ours fails
its condition and the AI falls through to the vanilla entries exactly as
before.

The game's own example of this is Mercer Frey in *Blindsighted*: a UseMagic
package in his alias's override list, gated on quest stage, makes him cast
Nightingale Strife at the player mid-fight. Ours is the same thing with a
faction rank as the trigger, because a faction rank is something the C++ can
set in one call and `GetFactionRank` is a standard condition function.

### The C++ (`src/game/Packages.cpp`)

1. **A cast rule fires.** Take a free record from the pool. Repoint its Spell
   input at the rule's spell and its Target input by the spell's **delivery**:
   a Self-delivery spell casts on her, anything else goes at the enemy she is
   engaging (a non-hostile targeted spell such as Healing Hands will need the
   player instead; not authored yet). Both inputs are found by canary, see
   below. Create a
   `RankLease`, whose constructor sets her rank in `FT_CastNow` to the slot
   number. Ask the AI to re-evaluate.
2. **The AI casts.** Our package now passes. `IgnoreCombat` takes her hands
   away from her combat AI; the UseMagic procedure interrupts what she was
   doing, charges, fires. Animation, cost and interruption are the game's.
3. **Release.** The tick destroys the lease on the first of: her animation
   graph emitting a spell-fire event **for our spell** (read from the spell
   equipped in the firing hand -- her own firebolts are logged and ignored);
   the AI having dropped the package; or a four-second deadline. The
   destructor clears the rank and re-evaluates, so she returns to fighting at
   once. The record goes back to the pool.

   A **concentration** spell (Flames, vanilla Healing) is a stream, and its
   fire event marks the *start*. Measured: releasing on it cut Flames off at
   0.35-0.65 s, whichever tick landed first. So for a concentration spell the
   fire event is not a release signal; the record's two `CastTime` floats are
   set to the sustain time (the rule's numeric argument, default 3 s) and the
   deadline is extended by it. The stream then runs for the time the record
   says, and the lease ends when the target dies, the AI drops the package, or
   at the deadline.

The **pool** is eight records with one holder each. A record is hers alone
until released, so nothing in it -- spell now, target later -- can be shared
by accident. When all eight are held, or she already holds one, her cast rules
report *busy* for that turn, spend no cooldown, and the next rule gets its
turn.

A follower carrying a rank while holding no record has a **stale** rank (a
save made mid-cast) and is cleared every tick; a game load drops the pool.

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
000B9986 rank 0 (leased, read back 0)
current package after evaluate: FE041800 (OURS)
anim 000B9986: right hand fired 000C969B "Firebolt" -- her own, ignored
anim 000B9986: left hand fired 0007231C "Fast Healing" -- OURS
packages: Marcurio releases slot 0 after 2.0 s: spell fired
000B9986 rank -1 (lease ended, read back -2)
```

| if instead | it means |
|---|---|
| alias: NO, or "in NO quest alias at all" | Recruited from the console. Only `SetFollower` fills the alias: use "Follow me", or `cqf DialogueFollower SetFollower <refid>` (unverified). |
| rank read back N INSTEAD | `AddToFaction` did not set the rank. |
| not ours yet -- watching, then deadline | Rank set, package never selected. Check the list order line at load. |
| OURS, then deadline with only her own spells firing | The package never got her hands. `IgnoreCombat` is off the records. |
| OURS, then deadline with nothing firing | She was staggered or otherwise stuck through the window. |

### Test procedure

1. `bat ftspawn`, click her, `bat ftmake` (grants Oakflesh, potions, relationship
   rank -- not teammate status).
2. **Talk to her, "Follow me."** The alias line in the log is the check.
3. A self-cast rule: `IF self health < 50% THEN cast Fast Healing ON self`.
4. `bat ftbear`, then read `FollowerTactics.log`.

Edits under `test/` do nothing until `tools\deploy-tests.ps1` runs: the game
reads the copies in the Skyrim root.

---

## Facts worth not rediscovering

Each of these cost at least one test round.

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
  `GetFactionRank` then reports -2. Either negative value means no slot
  condition passes.
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
  are read from authored records, not assumed. The template's name map spells
  it `SPELL`; compare case-insensitively. The map lives on the template, not
  on the copy.
- **The sensor must report whom she is fighting.** `currentCombatTarget` on
  the actor's runtime data. Without it, "cast at current target" resolves to
  no target and the rule never fires.
- **Copying a vanilla package copies its inputs.** Ours came from
  `MG07AncanoCastAtEye` and shipped aiming at the Eye of Magnus with a
  ten-million-second cast time. Read every input of a copied record.
- **`BGSRefAlias::ForceRefTo` does not exist in CharmedBaryon 3.7.0.** It does
  in alandtse's `ng` branch, which is what Simple Follower Framework uses to
  fill its own aliases from C++. Item for `docs/COMMONLIB.md`.

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
4. **`PutCreatedPackage` + `EvaluatePackage`.** Pushes onto the package stack,
   which combat does not consult. A quest at priority 99 would have failed the
   same way; only the alias's override list is read in combat.
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
| override-list splice + faction lease (shipped) | our records, the follower alias, our followers |
| a QUST at priority 99 | would not work: combat ignores the package stack |
| combat AI hooks | every actor in the game |
| GMST threshold | every actor, permanently |

---

## Follower frameworks, as reference

Read from their plugins with houseCARL. What matters is one alias field,
`CombatOverridePackageList`.

| framework | followers live in | combat-override list |
|---|---|---|
| vanilla | `DialogueFollower` alias 0 | `PlayerFollowerCombatOverridePackageList` (0005C852) |
| Simple Follower Framework 2.0.3 | follower 1 in the vanilla alias; 2..8 in `SFF_FollowerQuest` aliases 1..7, filled from C++ with `ForceRefTo` | the **same vanilla list** -- covered by the splice as-is |
| Nether's Follower Framework 2.8.6b | overrides `DialogueFollower` (nulls its list) and holds followers in `nwsFollowerPack` `PackAlias1..12` and tiers | `nwsFollowerCombatPkList` (007429), its own |

The splice is a table (`kOverrideLists`) with the vanilla list as its only
entry. NFF is one more line when integration is wanted. NFF also has
`nwsFollowerHealSelf` / `HealPlayer`: UseMagic packages gated on
`GetFactionRank` of an NFF faction, the same idiom.

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
- **An either-hand spell goes to the right hand by default.** Left alone by
  the first shadowing rule on the theory that the AI would keep it to the
  free hand, Flames went straight into the pinned right hand. Either-hand
  spells now compete with any pin.
- The seven arrays, from what appeared in them: [0] offence (attack spells,
  bows, blades), [1] restoration (healing spells and potions), [3] defence
  (wards, shields), [4] armour spells; [2], [5], [6] empty for a mage.
- With every usable right-hand spell set aside, he drew a dagger. The list
  is the list: the AI falls back to what is left, weapons included.

### How a pin is kept (2026-09-03, later)

By pruning the AI's list, not by touching her. Every tick she fights with
something pinned to a hand, every spell or item in the combat inventory that
would take that hand is erased from it, and whatever is pinned goes back in
its hand. A one-hand-only spell competes with a pin on its hand; an
either-hand spell, a both-hands spell, a one-handed weapon and a two-hander
compete with a pin on any hand; a shield or torch with a pin on the left. A
spell above her skill is never in the list to begin with, and cannot be
pinned, since the AI would not choose it. A pin changed mid-fight sets the
list's dirty flag so the AI rebuilds it whole and the next tick prunes it to
the new pins. The first version removed competing spells from her record for
the life of a pin, with a restore on unpin, dismissal and save; it worked,
and it left her without those spells for every menu, script and mod in the
meantime, which the prune does not. Each follower's list is her own: it
belongs to her combat controller.
