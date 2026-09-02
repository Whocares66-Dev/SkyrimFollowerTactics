# Making a follower cast a spell

Everything below was measured on **1.6.1170** with CommonLibSSE-NG 3.7.0. Seven
mechanisms were tried in one long session and none produced a rule-timed,
animated cast. The eighth -- a UseMagic package in the follower alias's **combat
override** list, gated by a faction rank -- is built and deployed but not yet
verified in game; it is described at the end. This file exists so nobody
repeats the first seven.

The short version: Skyrim has **no API that makes an NPC cast a chosen spell at a
chosen moment.** That is a design property, not a gap in the bindings. NPCs
decide for themselves, and every route in is indirect.

## Why a potion is easy and a spell is not

This is the one idea that explains every dead end below, and it is worth reading
before any of them.

**A potion is a state change. A cast is a performance.**

Drinking is an inventory operation. `ActorEquipManager::EquipObject` is the
game's own equip routine, equipping a potion consumes it, and the magic system
applies the effect at once. No animation to schedule, no AI decision, no state
machine. The engine exposes it because inventory manipulation is a normal
external operation -- quests, scripts and the player's own UI all do it.

Casting is something an actor *does over time*: charge, release, magicka drawn at
a particular instant, interruptible by a stagger, aimed at something. All of that
lives in the animation graph and the combat AI. There is no "perform this" entry
point because performing is not a state you can set.

The clincher is that the potion-equivalent for spells exists and works
perfectly -- `GetMagicCaster(kInstant)->Cast(...)` is reliable and instant. It is
exactly `EquipObject` for a potion: bypass the actor's agency and apply the
result. It simply is not a cast.

So the accurate statement is not "an NPC cannot be made to cast a spell". It is:
**the spell can be made to happen; the follower cannot be made to perform it.**
For a potion nobody notices the difference. For a spell the performance is the
whole point.

That is also why AI packages exist: behaviours go through the AI because
behaviours are the AI's job. Which is why the package route below is the
conventional answer and the recommended next path.

---

## What already works

| Action | Mechanism | Status |
|---|---|---|
| Drink potion | `ActorEquipManager::EquipObject`, NPCsUsePotions' parameters | works, measured in game |
| Equip spell | `ActorEquipManager::EquipSpell` | works; her AI then casts it **when it chooses** |

`Equip spell` is the honest shipping answer today: we choose *what* she holds,
her combat AI chooses *when*. It animates and it can be interrupted, because the
game is doing the casting.

---

## The seven dead ends

### 1. `CastSpellImmediate` / `Spell.Cast` — applies, never animates

Verified from three directions:

- The CK wiki: *"This function casts the spell instantaneously. This is mainly
  desirable only for non-actors, because **it will not animate an actor**"*, and
  it recommends *"an AI package with the UseMagic procedure"* instead.
- [DynamicAnimationCasting](https://github.com/LXIV-CXXVIII/DynamicAnimationCasting)
  — a plugin whose entire job is casting spells for actors — uses exactly
  `GetMagicCaster(CastingSource::kInstant)->Cast(...)`. It fires spells **on**
  animation events; the animation is already playing, driven by something else.
- `Actor.psc` has **no `Cast` function at all**. `spell.Cast(actorRef)` is on the
  *Spell* script, and `Actor::DoCombatSpellApply` applies an effect with combat
  die-rolls, not a cast.

Reliable, predictable, no gesture, cannot be interrupted.

### 2. Animation events — outputs, not inputs

`MRh_SpellReady_Event` / `MRh_SpellFire_Event` are real, and it is tempting to
send them. They are the wrong direction.
[NPC Spell Variance](https://github.com/LeoneKingzz/NPC-Spell-Varaince-Unified)
settles it — it hooks `ProcessEvent` and *receives* them:

```
RECEIVED (the game emits): BeginCastLeft, BeginCastRight,
                           MLh_SpellFire_Event, MRh_SpellFire_Event, InterruptCast
SENT     (the mod drives): attackStop, recoilStop, bashStop, blockStop,
                           staggerStop, InterruptCast
```

Note `InterruptCast` is in **both** lists: interruption *is* a supported input.
Starting a cast is not.

That mod is also the best evidence for the general shape of the problem: a
production plugin dedicated to NPC spell usage **never initiates a cast**. It
waits for the AI to start one and then steers `caster->desiredTarget`.

### 3. `SetCurrentSpellImpl` + `RequestCastImpl` — inference, never verified

An attempt to drive the caster's state machine directly. These are `Impl`
virtuals that the game's own update loop calls, no implementation could be found
that calls them from outside, and it was never demonstrated to work.
`docs/PLAN.md` line 451 had already said to verify this API before designing
around it. Recorded here as a warning, not a technique.

### 4. UseMagic AI package — correct route, pushed onto the wrong stack

The documented answer, and the one the CK wiki points at. It got further than
anything else and still failed:

```
current package 000B9987 -> 000B9987 (not ours -- outranked)
```

`Actor::PutCreatedPackage` + `EvaluatePackage(immediate)` pushes the package, and
the actor's own package keeps running. `createdPackage=true` made no difference.

At the time this read as a *priority* loss. Reading the records later showed it
was not: an actor in combat does not consult her package stack at all. See "The
eighth attempt" below -- the package route is the one that now ships, just not
through the package stack.

### 5. Swapping `CombatMagicCaster::magicItem` — wrong level

The caster holds `inventoryItem` (the AI's catalogue entry) and `magicItem` (the
spell). Writing `magicItem` "took" in memory and changed nothing useful, because
the caster in question had selected a **potion**:

```
magicItem Potion of Magicka 0003EAE1 -> Fast Healing 0007231C (write took)
inventoryItem->GetMagic() = Potion of Magicka 0003EAE1
```

`AlchemyItem` derives from `MagicItem` and `CombatInventoryItemPotion` derives
from `CombatInventoryItemMagic`, so potions run through the same machinery.

### 6. Boosting `CalculateScore` — unnecessary, the spell already wins

The plan was to make our spell out-score the alternatives. Measurement killed it
in one run:

```
[spell]  Fast Healing             222.750
[potion] Potion of Healing         50.000
[potion] Potion of Minor Healing   25.000
[potion] Potion of Magicka         16.500
[potion] Potion of Minor Magicka    8.250
```

All scored in a single pass, so it is one contest — and the spell already leads
by 4.5x. Boosting would have "worked" and changed nothing.

Worth keeping: the scale is **hundreds**, not 0..1, and the score is constant
across health levels, so it is intrinsic to the item.

### 7. `CombatMagicCasterRestore::CheckStartCast` — the AI never asks

The most promising lever. The combat AI asks itself *"should I start a restore
cast now?"* through vfunc 6, and answering `true` gives rule-timed casting
through the game's own path, with animation and interruption. The hook installs,
is entered, and is safe.

It fails on something upstream:

```
combat-hook: a restore caster asked, primaryAV=25 (magicka)
queued:  2
REQUEST: 0
```

**One restore caster exists per actor value** (`primaryAV`). Only the magicka one
ever asked. The health caster never evaluated at 43% health — while the same
follower's AI healed itself from 1 HP earlier in the session, so the threshold is
real and very low.

We can answer the AI's questions. We cannot make it ask one. Interrupting is
unlikely to help: an actor whose threshold says "not hurt enough" will re-decide
the same way.

---

## Facts worth not rediscovering

- **Two spells can share a display name.** Marcurio's heal is `0007231C`; the
  vanilla one is `0002F3B8`. Both are called "Fast Healing". Compare FormIDs.
- **The UseMagic package template's named inputs** (uid = name):
  `0 Place to Travel, 1 destination, 2 Location, 3 SPELL, 4 Target,
  5 HoldWhenBlocked, 6 CastTimeMin, 7 CastTimeMax, 8 CooldownTimeMin,
  9 CooldownTimeMax, 10 NumToCastMin, 11 NumToCastMax, 12 DualCast`
  Note the map spells it **`SPELL`**, not `Spell` — compare case-insensitively.
- **The name map lives on the template**, not on packages built from it. A copy
  carries the values and a `templateParent` pointer.
- **Where a package's spell lives**, found by canary rather than guessed:
  `PTDA - Target \ Target Data \ Target` (Type = Object ID), and in memory at
  `IPackageData + 0x10 -> + 0x08`. Ints are at `CNAM - Value \ Integer`.
  Both parents are structs/unions that reject assignment.
- `MG07AncanoCastAtEye` has `NumToCastMin = 1000`, which is why Ancano casts
  forever. Copies want `1` so the package completes and normal AI resumes.
- **Dual casting is a native package input** (uid 12), so it comes free on the
  package route. Via the caster API it needs a perk most followers lack and
  multiplies the cost (~2.8x).
- `CombatController::cachedAttacker` is a **cache of** `attackerHandle`. Read the
  handle, not the cache — the cache returned `0x1` and crashed us.
- CommonLibSSE's `static_assert(sizeof(...))` confirms a struct's **size**, not
  that a named field means what it says in every state. These headers are
  reverse-engineered from a shipped binary.

---

## The eighth attempt: the combat override list

Implemented 2026-09-02 and **verified in game the same day**: with Marcurio
recruited through dialogue, a rule at 43% health armed slot 0, the AI selected
our package on the spot, and his health went 75 -> 175 about two seconds later:

```
alias: quest 000750BA "DialogueFollower" alias 0 "Follower"  <- follower alias
000B9986 rank 0 (armed, read back 0)
current package after evaluate: FE041800 (OURS)
... health 175/175 (100%)                                    2.2 s later
```

Rule-timed, animated, through the game's own cast path. The first run's
failure was the harness: the game was running a stale copy of `ftmake` that
recruited from the console, and a console teammate is not in the alias.

What the first success also showed: the package **does not complete** after
its one cast. Four seconds on, `GetCurrentPackage()` was still ours and he
stood idle -- "he healed and then froze". Two changes from that:

- `IgnoreCombat` is **on**, after a detour. It was removed because he stood
  idle after healing; the idling turned out to be the missing re-evaluate on
  release. Without the flag (12:59 run) his combat AI kept dual-casting
  Firebolt and Lightning Bolt with our package current, and the heal never
  got his hands. With it (11:43 and 12:21 runs) the heal landed in 1-2 s.
  The flag is what makes the package take his hands; the release is what
  gives them back.
- The record is released on the **spell-fire animation event**
  (`MRh_SpellFire_Event` / `MLh_SpellFire_Event`, via a sink on the caster),
  and every release calls `EvaluatePackage(immediate)` so the AI leaves the
  package at once instead of at its own leisure. The 4 s window remains as
  the backstop.

Second run, same afternoon, with those in: four requests, **one heal**. The
sink was releasing on the first fire event it saw, and Marcurio is a
Destruction mage -- three of the four were his own firebolt, 65-216 ms after
arming, and the early release dropped the rank and cancelled our package
before it cast. The one that healed was the one where his hands happened to
be free. So the sink identifies the spell and accepts only ours. Every fire
event while a record is held is logged with the spell it was.

**Working end to end, 13:15 the same day.** Two consecutive cycles, each:
package selected on the tick the rank was set; `left hand fired 0007231C
"Fast Healing" -- OURS` 1.4 s later; record released on the next tick; health
75 -> 175. His own Firebolts before and after were logged and ignored. The
sequence of runs that got there is kept below because each one changed the
design.

Third run: two heals (1 -> 114 on one of them), but every fire event read
spell `00000000` -- `MagicCaster::currentSpell` is already null when the
event arrives, so nothing matched and every lease ran to the deadline. The
sink now reads the spell **equipped in the firing hand**
(`selectedSpells[hand]`), which the UseMagic procedure sets and which is
still set at fire time. Also seen: four cast rules fired at negative health
while he was in bleedout with no potions left. Evaluation is now held while
a follower is bleeding out.

### What the data layer showed

Reading the records (with houseCARL, against the live load order) changed the
diagnosis of dead end 4. The pushed package did not lose on *priority*. It lost
because **an actor in combat does not run her package stack at all.** She runs
the **Combat Override Package List** on her quest alias, and the vanilla
follower alias already has one:

```
DialogueFollower (000750BA), priority 50
  alias 0 "Follower"
    PackageData               = [PlayerFollowerSayDismissPackage, PlayerFollowerPackage]
    CombatOverridePackageList = PlayerFollowerCombatOverridePackageList (0005C852)
      [0] PlayerFollowerCombatOverridePackageExterior   HoldPosition, IsInInterior == 0
      [1] PlayerFollowerCombatOverridePackage           HoldPosition, no conditions
```

Top to bottom, first passing condition wins. So the quest at priority 99 the
previous section asked for would have put a package on the *package* stack,
which is exactly the stack combat ignores. It would have failed the same way.

The game's own worked example of what we want is **Mercer Frey**. In
*Blindsighted*, while fighting the player, he casts Nightingale Strife *at* the
player on cue -- and the cue is a quest stage:

```
TG08B alias "MercerAlias"
  CombatOverridePackageList = TG08bMercerWithdrawCombatOverride
    ... TG08BMercerCombatOverrideCastAtPlayer (0FDBC3)
          template UseMagic, flags IgnoreCombat
          conditions: HasSpell(Nightingale Strife), GetStage(TG08B) >= 40, < 45
          Spell = Nightingale Strife, Target = PlayerRef
          Location = NearSelf r10000, CastTime 0.5..1, Cooldown 1..1, NumToCast 1..0
```

A condition that changes mid-fight, re-evaluated mid-fight, producing an
animated cast. That is the whole feature, shipped in the base game, and the
community answer for follower support spells is the same thing: a UseMagic
package in a form list in the alias's Combat Override slot
([Nexus forum](https://forums.nexusmods.com/topic/12728355-how-do-seperate-spells-and-combat-override-list-for-npc/),
[Interesting NPCs](https://3dnpc.com/2013/11/18/creation-kit-combat-ai/) does
it for Valgus's heal-other).

### What was wrong with our packages

They were copies of `MG07AncanoCastAtEye`, and they still carried Ancano:

| input | shipped as | meaning |
|---|---|---|
| Target | `MGEyeCollegeRef` | cast at the Eye of Magnus |
| Location | `MG08AncanoMarker` r500 | walk to the College first |
| CastTimeMin/Max | 10 000 000 / 100 000 000 | channel forever |
| DualCast | true | needs a perk, ~2.8x cost |
| Flags | none | |

Had the package ever won, she would have set off for Winterhold. This is why
"the package was pushed and nothing happened" was never going to be
diagnosable from the package side.

### What the ESL holds now

`esp/FollowerTactics.esp` (now versioned; the xEdit generator in
`tools/xedit/` is superseded and kept for its notes). ESL-flagged, master
Skyrim.esm:

| record | FormID | contents |
|---|---|---|
| `FT_CastSlot1..8` | 0x800..0x807 | UseMagic: Spell = canary, Target = **Self**, Location = NearSelf r10000, CastTime 0.5..1, Cooldown 1..1, NumToCast 1..1, DualCast off, flags IgnoreCombat, condition `GetFactionRank(FT_CastNow) == slot` |
| `FT_CastNow` | 0x808 | faction, ranks 0..15 |

Every record was written and read back by houseCARL against the Mutagen
schema, and the plugin passes a dangling-reference sweep.

### What the C++ does

`src/game/Packages.cpp`:

- **At load** it puts the eight slots at the **front** of
  `PlayerFollowerCombatOverridePackageList` in memory. Front, because entry
  [1] has no conditions and nothing after it is ever reached. In memory, not
  in the ESP: no override of a vanilla record, no conflict with anything else
  that touches it, and it is redone on every launch.
- **When a cast rule fires** it takes a slot from the pool, repoints its
  Spell (the probe from attempt 4, unchanged), sets the follower's rank in
  `FT_CastNow` to that slot, and calls `EvaluatePackage(immediate)`.
- **Every tick** it watches `GetCurrentPackage()`. When ours has run and is no
  longer current, or 4 s have passed, the rank is set back to -1 so no slot's
  condition passes and the list falls through to vanilla, and the slot is
  released.

The eight records are a **resource pool with one holder each**. A follower
takes a free record when her cast rule fires, it is hers alone until the cast
has run or the window has passed, and then it goes back. Records are never
shared, even between two followers casting the same spell: every input in the
record (spell now, target later) belongs to the holder, so the pool cannot be
caught out by an input it did not think to compare. The limit is eight
followers mid-cast at the same instant. When that is exceeded, `HasFreeSlot()`
is false, the rule engine marks cast rules `Busy` for that evaluation only, no
cooldown is spent, and the next rule down gets its turn. That path has a unit
test; the pool itself does not, because it touches the game.

**How a record always comes back.** The rank is owned by a `RankLease` in
the slot: its constructor sets the rank, its destructor clears it and asks the
AI to re-evaluate, and the only way to free a record is to destroy the lease.
That makes the cleanup unskippable. It does not make it prompt -- nothing in
C++ ends the lease by itself -- so the tick does, at the **deadline** (4 s
after arming) whatever the game did, or earlier on one of two signals, so a
follower is not held for four seconds after a one-second cast:

| signal | meaning |
|---|---|
| spell fired | `MRh/MLh_SpellFire_Event` reached the sink: the effect is on its way |
| package ended | ours was current and no longer is: the AI already moved on |
| deadline | neither arrived; release anyway and say which |

Deadlines and cooldowns are measured on **game time** converted to real
seconds at the timescale. It stops in menus, so a request armed just before
the panel opens is exactly as old when it closes (the 12:59 run found leases
"expired after 25 s" on the wall clock after the panel had been open), and it
jumps on wait, sleep and fast travel, which expires every cooldown.

Two states fall outside a held record: an actor whose handle no longer
resolves is dropped (nothing to clear a rank on), and a game load drops the
whole pool. Both are covered by the stale-rank sweep below.

The sweep: every tick, any managed follower carrying a rank while holding no
record has it cleared, with a warning. That is what stops a save made mid-cast
from loading a follower who passes her slot's condition on every evaluation
for the rest of the session -- a leak that would never show up as an exhausted
pool.

Nothing about the combat AI is hooked. `kEnableCombatHooks` stays off.

### The log lines that decide what happened

Each fired cast rule produces, in order:

```
Lydia in the DialogueFollower alias: yes | NO -- recruit her through dialogue
FF000DE0 rank 0 (armed)                        <- or "read back N INSTEAD"
current package after evaluate: xxxxxxxx (OURS | not ours yet -- watching)
packages: Lydia is RUNNING slot 0 (spell 0005AD5C)
packages: slot 0 completed
FF000DE0 rank -1 (cast completed)
```

and if it does not go that way:

| line | what it means |
|---|---|
| alias: NO | She is a teammate but not the alias's follower. The override list was never consulted. Recruit with "Follow me"; the console `setplayerteammate` route does not fill the alias. |
| rank read back N INSTEAD | `AddToFaction` did not set the rank. Conditions can never pass. Fall back to setting the rank another way. |
| expired, never picked up | Alias yes, rank yes, condition never selected. Suspects, in order: the list order in memory (logged at load), `IgnoreCombat`, `HoldWhenBlocked`, the spell failing `CheckCast`. |
| RUNNING but no animation | The package ran and the UseMagic procedure declined. Most likely an unaffordable or non-self spell. |
| FIRED the spell ... releasing | The cast happened. This is the line that means success. |
| expired ... was still running | The cast may have happened, but no fire event reached the sink. Check the `anim` lines for what the graph did emit. |

### Test procedure

The harness in `test/` used to recruit with `setplayerteammate 1` and
`addtofaction CurrentFollowerFaction`. That makes a *teammate*, not an *alias
follower*, and the alias is what carries the override list. So:

1. `bat ftspawn`, click her, `bat ftmake` (grants Oakflesh, potions, sets
   relationship rank). It no longer sets teammate or faction.
2. **Talk to her and choose "Follow me."** That is `SetFollower`, which fills
   the alias.
3. Author a rule: `IF self health < 90% THEN cast Oakflesh ON self`, or any
   self-cast spell she can afford.
4. `bat ftbear`, watch her, then read `FollowerTactics.log`.

### Follower frameworks

Read from the frameworks' own plugins with houseCARL, 2026-09-02. What matters
is one field per alias, `CombatOverridePackageList`, because that is the list
our packages have to be in.

| framework | where followers live | combat-override list | our splice |
|---|---|---|---|
| vanilla | `DialogueFollower` alias 0 | `PlayerFollowerCombatOverridePackageList` (0005C852) | yes |
| Simple Follower Framework 2.0.3 | follower 1 stays in the vanilla alias; 2..8 in `SFF_FollowerQuest` (priority 51) aliases 1..7 | **the same vanilla list** on every extra alias | covered already |
| Nether's Follower Framework 2.8.6b | overrides `DialogueFollower` (nulls its list, adds `FollowerExtra1..10`) and holds followers in `nwsFollowerPack` `PackAlias1..12`, plus `_High` / `_VHigh` tiers | `nwsFollowerCombatPkList` (007429), NFF's own fifteen HoldPosition variants | spliced when NFF is loaded |

The splice is a table (`kOverrideLists`) with the vanilla list as its only
entry today. Covering NFF is one line once integration is wanted; SFF needs
nothing. Which alias's list the engine consults when a follower is in several
(NFF puts her in two quests) is not verified.

SFF is also the answer to the "our own quest" question. It is an SKSE plugin
on the current CommonLibSSE-NG, and it fills its aliases with
`BGSRefAlias::ForceRefTo(actor)` -- a library call that **our** CommonLib
(CharmedBaryon 3.7.0) does not have. That is a concrete item for the fork
migration in `docs/COMMONLIB.md`, and it makes a FollowerTactics quest with
its own aliases a small job once the library is current: eight aliases, each
pointing at our own list, filled for any teammate we manage. Then no
framework's choice of alias matters.

### Still open

- **Targets other than self.** The rule engine already resolves Player and
  CurrentTarget; the pool refuses them (`NotSelfTarget`). The Target input is
  in the record like the Spell input. Because a record has one holder at a
  time, repointing the target per request is the same shape of change as
  repointing the spell: one more unmapped write, found by canary, and no
  change to the pool.
- **Followers outside the known lists.** EFF and AFT have not been read.
  Adding one is one line in `kOverrideLists`, once its alias's list is known.
- **A second cast in the same window.** Disarm happens on the next tick after
  the package completes; if the AI re-selects within 150 ms she may cast twice.
  Watch for it before fixing it.
- **Creating packages at runtime** would remove the number eight entirely, and
  is not worth it: the package data is a templated structure CommonLibSSE does
  not map, and a shallow clone sharing a pointer with its source would fail
  only when two of them were written. Eight records cost nothing.

### Rejected: changing the health-restore threshold

If the threshold that stops the health caster asking is a game setting, changing
it is technically small. It is also **global** -- it would change when every NPC
in Skyrim heals, in order to make our followers heal on time. Wrong trade for a
follower mod, and recorded as rejected so it is not rediscovered as a cheap idea.

| approach | reaches |
|---|---|
| equip spell, instant apply | one follower, one action |
| combat-override splice + faction gate | our records, the follower alias, our followers |
| a QUST at priority 99 | would not have worked: combat ignores the package stack |
| combat AI hooks | every actor in the game |
| GMST threshold | every actor, permanently |
