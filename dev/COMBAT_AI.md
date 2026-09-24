# The combat AI: how it chooses, and what tunes it

Read from the disassembly of 1.6.1170 (AE Address Library IDs) on 2026-09-22, branch `wip-scoring`. **Nothing here is verified in play yet.** Each claim is READ (seen in the instructions) or INFERRED; unmarked means READ. GMST values are the executable's compiled defaults; whether Skyrim.esm or an ini overrides any of them is not checked. The working notes, with raw dumps and helper scripts, were in `build/scoring/` (untracked): `score-functions.md`, `selection.md`, `casters.md`.

It answers three questions: why a follower always casts the same spell, why cloaks are cast so eagerly, and why Fire Storm is never cast. And it names where a different chooser (a recency penalty, a weighted random pick) could go in.

## The short version

- **Nothing on the choosing path is random.** The engine's RNG (68276, seeded by 68275, 266 callers) is not called from the inventory code (44836-45100), the equip context (48117-48121), or their direct callees. Randomness in combat decides *when* to act (cast chance, dual-cast chance 49095), not *what* to hold.
- **It is not one argmax over everything.** Once a second the inventory is rescored and a loadout built by walking the categories in a **fixed order**, taking the best-scoring entry of each that still fits a free hand. Categories are never compared with each other by score; a score only competes inside its own category.
- **Buffs get a hand before attacks do.** Restore, wards and long buffs (cloaks among them) are offered a hand ahead of offence. Their gates, not their scores, decide whether they take it.
- **Inside offence, the same thing wins every time.** Melee, bows and attack spells are all category 0 and compete on score; the score is deterministic and has no memory of what was used, so the top entry stays on top until magicka, range or a gate says otherwise.

## 0. Which spells get into the list (read 2026-09-22)

The combat inventory is built at a fight's start from the actor's spells and items. A spell is made into an entry (44893, then 44898) only if all three hold:

- **Every effect with a magic school is within the caster's skill in that school.** The visitor 45328, over the spell's effects: the effect's skill (`EffectSetting+0x78`) against its minimum skill (`+0xA8`). One effect short, and the whole spell stays out. So a mod's extra effect can keep a spell out: Adamant's Permafrost (Destruction 75) on Ice Storm kept Serana's Ice Storm out at Destruction 50-odd, while Chain Lightning, whose added effects have no minimum, was in at 50. The mod's own mirror of the rule (`AboveSkillForAI`, the Magic tab's dimming and the rules' "above skill") reads every effect the same way; until 2026-09-22 it read only the costliest one and called Ice Storm usable.
- **At least one effect has a scoring entry** (45320; section 2).
- **Its cost is within the caster's maximum** (45323 against `GetPermanentActorValue` of the cost's actor value): magicka for a spell. A staff's charge (`RightItemCharge` 64, `LeftItemCharge` 82) is exempt here.

**A staff and its charge (read 2026-09-23).** When the engine counts an actor's items for the combat list (44884, `SetItemCount`; inlined in the rebuild 44879), a staff copy (a weapon of type 8) whose own charge, `ExtraCharge` on the copy, is **below its enchantment's unscaled cost** is not counted. With no copies counted, the loadout skips the staff whatever it scores (44899's count check). The threshold is `CalculateMagickaCost` with **no caster** (11321): the record's cost where the enchantment is flagged no-auto-calc, else the sum of its effects' base costs, with no perks. What a cast actually draws is the cost **with** the caster's perks (the entry's 125 below), so between the two a staff can still be cast and is ignored. Serana's Rahgot's Staff (`MAG_BloodLanceStaffEnch`, cost 250, no-auto-calc; 125 a use with her perks) was ignored at 238 charge and equipped at once at 2638. A copy with no `ExtraCharge` (never used, so full) always counts. **Fixed for every actor** (2026-09-23, `src/fix/StaffCharge.cpp`): both calls are rewritten to ask the cost of the actor whose inventory it is, the cost a use is actually charged, so the AI keeps a staff while it can cast it; not yet seen in play. The count is taken at a fight's start and whenever the combat inventory is rebuilt. The hand charge values (`RightItemCharge`, `LeftItemCharge`) play no part here. They are the per-use cost's resource in the loadout, where an unequipped staff has an unlimited budget (44908, `FLT_MAX`).

## 1. When it decides: `CombatInventory::Update` (44858)

Called every AI update from `CombatController::Update` (33217), itself from the actor update (38565).

```
CombatInventory::Update(inv):                                  // 44858, 0x80f380
    if now - inv.lastScore (+0x194) <= inv.interval (+0x198): skip scoring
    for each of the 7 arrays, for each entry:
        entry.itemScore (+0x18) = entry->vfunc[0x0C](controller)   // call at 0x80f40e; the slot we hook
    sort each array by 44909: score descending, ties by FormID
    BuildEquipmentSets(inv)                                    // 44899, call at 0x80f55d
    inv.interval = 1.0                                         // -1 after a rebuild: next pass immediate
```

Combat start (44857) calls 44899 too (call at 0x80f2fd), and sets `CombatInventoryItemOneHandedBlock`'s score to a constant 1.0.

## 2. The score (vtable slot 0x0C)

### Spells, staves, shouts, scrolls, potions: 45082 -> 45084

```
MagicScore(item, ctrl, magicType, maxRange):                   // 45084, 0x819c10
    if !item: return -1                                        // READ; 186426 is the start value, -1
    data = CombatMagicItemData(ctrl, item)                     // 45319
    if !data.FindBest(): return -1                             // 45320: no effect with a registry entry
    mult = style.magicScoreMult (+0x30)   if spell             // magicType 4
         = style.shoutScoreMult (+0x38)   if shout             // 5
         = style.staffScoreMult (+0x40)   if staff             // 6
         = 1.0                            otherwise (potion, scroll)
    if maxRange > fCombatRangedDistance (1024): mult *= fCombatInventoryRangedScoreMult (2.0)
    return mult * data.total (+0x48)
```

`maxRange` comes from 47332: min(projectile range x 0.9, 4096), or 4096 for target-actor and target-location delivery; self and touch spells appear to stay at 0 (INFERRED), so in practice only aimed spells get the x2.

`data.total` is the **sum over the spell's effects** that have a registry entry (visitor 45321, one effect scored by 45325). The single best effect only decides whether the spell counts at all and which caster class it gets.

```
EffectScore(effect):                                            // 45325
    key = archetype<<16 | actorValue<<8 | hostile<<1 | spellIsSelfDelivered
    entry = registry[key]                                       // global 405245, from table 382289
    if !entry: return 0
    dur = max(duration, 1)
    if concentration: dur = fCombatMagicConcentrationScoreDuration (3.0)
    dur += taperDuration * taperWeight * 0.5
    s = entry.scoreFn(entry.magFn(effect) * dur) * entry.mult
    (the effect's second actor value is scored again, weighted by secondAVWeight)
```

- `magFn`: 45452 is the record's magnitude; 45453 is the effect's base cost.
- `scoreFn`: 45454 multiplies by the target's resistance, but only for hostile effects with a resist value. 45455 (damage magicka or stamina) does the same, then returns 0 unless the target's value is above 20% and the target is a caster (magicka) or a melee fighter (stamina). 45456 (Script) returns the effect's AI score, ignoring magnitude and duration.
- `mult` by kind: damage health 1, magicka 0.5, stamina 0.33. Stagger, disarm, command summoned and reanimate 10. Banish 5. Turn undead 3. Paralysis 20, on cost x duration. Restore health 1, magicka 0.33, stamina 0.1. Ward, armour, cloak and light 1, on magnitude x duration. Summon, invisibility and bound weapon 1, on cost x duration.
- Each ValueModifier entry is also registered under Absorb, DualValueModifier, AccumulateMagnitude and PeakValueModifier (45326), **so Absorb Health scores as Damage Health**. This corrects `dev/MAGIC.md`.
- **Not in the score:** perks, skill, magicka cost, the caster's own health or magicka, the target's health, distance (beyond the x2 over 1024 units), time since last use, randomness. One gate sits outside it: at build time (44898), a spell costing more than the actor's maximum magicka is never made into an entry.

The table itself, and why a hostile spell centred on the caster (Fire Storm) scores nothing, are in `dev/MAGIC.md` "Which spells the combat AI can use at all". Serana's Cold Fire Storm read 0.00 there, where 45084 would give -1 for no entry; it is probably a matched Script effect whose AI score is 0 (INFERRED).

### Weapons

```
Melee (45015)  = DPS(actor, weapon, noAmmo, style) - (leftHand ? fCombatInventoryDualWieldScorePenalty 0.01 : 0)
Ranged (45031 -> 45033) = no ammo ? 0 : DPS(actor, weapon, ammo, style) * (maxRange > 1024 ? 2.0 : 1)
Shield (45045) = armourRating * Block / 100
Torch  (45065) = Block / 100
OneHandedBlock (45054) = its stored itemScore, a constant 1.0

DPS (26416):
    if weapon is flagged NotUsedInNormalCombat: return 0
    d = WeaponDamage(26410)  // base + ammo, x fDamageWeaponMult, x skill scaling
    d *= speed * (bow ? fCombatDPSBowSpeedMult 0.2 : fCombatDPSMeleeSpeedMult 1.0)
    if unarmed and the race can hold weapons: d *= fDamageUnarmedPenalty (0.2)
    d *= style.meleeScoreMult | rangedScoreMult | unarmedScoreMult
    if bound: d += fCombatBoundWeaponDPSBonus (100000)
```

What the weapon damage (26410, called through 26411) reads, and what it does not:
- **Reads:**
  - the weapon's base damage, plus the ammo's (+0x11C), times 374110;
  - skill scaling: a lerp between two GMST pairs by the clamped skill value (27284), one pair for the player, one for NPCs; so a Fortify skill effect counts;
  - the MeleeDamage or UnarmedDamage actor value (0x22, 0x23);
  - one conditional factor (410199; not named).
- **Does not read:**
  - perk entry points: no call to `BGSEntryPoint::HandleEntryPoint` (23526), so Armsman, Barbarian and Overdraw are not in it;
  - tempering and enchantment: the entry holds the form, not the copy;
  - the target: no target is passed, so its armour, race and immunities play no part.

Spells are scored against the controller's current target. `scoreFn` 45454 calls `IsHostile` (11013), then `MagicTarget::CheckResistance` (virtual 0x0A). So a fire-immune target zeroes a fire spell's effect, and the rescore once a second follows a change of target. An immunity that is not a resistance value (an effect condition, a keyword) is not in the score. The Offensive gate does not check "valid" either, so nothing stops a condition-blocked attack spell being chosen (INFERRED from the gate table).

**The units differ:** weapons score damage per second, spells score magnitude x seconds. They only meet inside category 0, where the combat style's multipliers are the one lever that weighs them.

## 3. The loadout: `BuildEquipmentSets` (44899, 0x8134c0)

It fills two `CombatEquipment` sets. CommonLib has the struct's layout wrong: `score` is at +0x1C, then max, optimal and minimum range.

- **Set A (+0x118)** takes the raw score. It decides which range to fight at.
- **Set B (+0x148)** takes a range-adjusted score (44920 and 44864: x0.1 when the target is out of reach or inside the minimum range). **Set B is what gets equipped.**

```
for category in [1, 2, 4, 0, 3, 5, 0, 6]:        // table 382271 (RVA 0x20162c8, count 382272); READ
    queue = entries of category (slot 0x0B), keyed by
            score + (held ? fCombatInventoryEquippedScoreBonus 0.1 : 0) - index * 1e-4
    drop keys <= 0
    pop best first; admit an entry only if
        its slot bits (hands, voice) are still free
        and a copy is left
        and CheckShouldEquip (slot 0x0F) passes            // the caster's gate, section 4
        and it can pay: cost x fCombatInventoryResourceDesiredRequiredMult < what is left
            (reserve x2 for set A, x1 for set B), then the cost is taken off
    offence that cannot reach the set's range needs >= 10% of the set's score
        (fCombatInventoryEquipmentMinScoreMult), else it is deferred
    the first category-0 pass takes one item; the second, and 6, may take more
```

Categories, from the caster's slot 0x0B:
- 0 offence: attack spells, melee, bows.
- 1 restore.
- 2 and 3 wards and defence: concentration ward, shield, torch. The split is at `fCombatMagicBuffDuration` (30 s). Which of the two holds the long ward is not settled: the two readings disagree.
- 4 and 5 the other buffs, split at `fCombatMagicTacticalDuration` (30 s of effect duration, `item+0x38`). Disarm and light are always 4. Script is 4 at 30 s or more, otherwise 0.
- 6 one-handed block.

Set A's ranges win over set B's only if set A's score is greater than set B's x a multiplier (1.5 to 10) set by the combat style (50694; INFERRED).

## 4. The gates (`CheckShouldEquip`, slot 0x0F; the cast checks, slots 06-0C)

Every `CombatInventoryItemMagicT<X, Caster>::CheckShouldEquip` is `Caster::StaticShouldEquip(ctrl, item)`, shared by the spell, scroll, staff, potion and shout variants.

The shared checks:
- **afford** (45342, 45347): current magicka > cost.
- **active** (45344): the caster already has the spell.
- **affected** (45349): the target already has it. Both skip concentration spells.
- **valid** (45343): the effect's conditions pass and the target can take it.
- Restrict timers are per combatant (set by 46559). `inventory+0x1b0` is the effective distance to the target (written by 50693), not a weapon range as CommonLib's name suggests.

| Caster | Equip gate | Cast / stop / cooldown |
|---|---|---|
| Offensive 45350-56 | not fleeing, **nothing else** | Start: afford; concentration has a cooldown. Cast chance lerps 0.05-0.75 (fire-and-forget), 0.25-1 (concentration). Hold 0.5-1.5 s. Concentration: cast 4-10 s, then wait 0.5-2.5 s. **No cooldown for fire-and-forget** |
| Restore 45371-75 | 15 s timer (magicka 10 s), afford, not active, valid, health % < lerp(0, 0.5, style's defensive mult +0x24), +0.25 while already restoring (magicka: 0.25-0.4); always yes when the target is bleeding out | Stops 2 s after health is back past threshold + 0.25; the timer starts then |
| Ward 45361-63 | magicka % > 0.5, not active, valid, an enemy within 384 | A short ward starts only against an incoming melee or projectile; stops below 25% magicka or 1 s after the threat |
| **Cloak** 45398-400 | not fleeing, afford, **not active**, and distance < R, or (duration >= 30 s and the enemy's reach < R); R = magnitude x 22 + 128 | Stops when distance > magnitude x 22 + 384. **No timer, no magicka %** |
| Armour 45416-18 | not fleeing, afford, not active, the enemy is armed; duration >= 30 s, or distance < 256 + reach | Stops when distance > 512 + reach |
| Summon 45384-86 | not fleeing, 30 s timer, afford, not active (the summon is still up), valid | The 30 s timer starts when the summon ends |
| Bound weapon 45412-13 | must outscore the best offence entry (less the 100000 bonus), distance < 1024 + reach | afford, not active, valid |
| Invisibility 45408-09 | afford, not active; fleeing, or distance > 1024 with a melee weapon held | afford, not affected |
| Light 45403-05 | a combat-group check (INFERRED: searching), not fleeing, afford, not active, it is dark | Stops when that check fails |
| Stagger 45389-90 | not fleeing | afford, not affected, valid; the enemy is attacking, casting, or within 256 + radius |
| Disarm 45393-95 | not fleeing, 30 s timer, the enemy has a weapon, distance < 256 + its reach | 30 s timer after a cast |
| Paralyse 45443-45 | not fleeing, 30 s timer, afford, not affected, valid, distance < 384 + max(both reaches) | 30 s timer |
| Target effect 45424-25 | not fleeing, afford, not affected, valid | the same |
| Script 45448-50 | not fleeing, a timer (the effect's AI delay time), afford, not affected, valid | afford, not affected |
| Reanimate 45432-34 | not fleeing, 30 s timer, afford, not active, a corpse is found | similar; the target is the corpse |

The cast checks' call sites in the behaviour tree (built by 49112) are not traced; that the tree calls them is INFERRED.

## 5. The equip: `CheckEquipmentChanged` (48118, 0x877a40)

A per-slot behaviour-tree node (tree built by 47688, "Action Equipment Dynamic Conditional Node") switches to what set B holds for its slot. **44859 CanEquip** can block the switch:
- while `fCombatInventoryMinEquipTime` has not passed (table 382277: weapon 5 s, magic 3 s, shout 3 s, staff 5 s, block 5 s; applies while drawn, INFERRED);
- or while the held item reports itself busy (slot 0x0E).

44860 records `{item, equipTime}` in `inventory->equippedItems` and calls Equip (slot 0x11).

**Hysteresis, all of it:**
- The +0.1 bonus for the held item. With scores in the tens to hundreds, it only breaks ties.
- In set B, +384 reach for the held item and +256 minimum range for items not held.
- The 3 to 5 s minimum equip time.
- The 1 s rescore.

## 6. Dual casting (read 2026-09-23)

The combat AI dual-casts on its own; nothing of ours asks it to. The magic behaviour tree (49112) runs, per hand's magic context and for either casting type, a fallback over three children, re-rolled each cycle (the re-roll INFERRED):

```
1. RandomNode(p = CalcDualCastMagicChance 49095) -> PrepareDualCast (49110/49111) -> cast with dual = true
2. RandomNode(p = CalcCastMagicChance 49094)     -> the single cast
3. Idle 0.25 s
```

- **The chance** (49095, checked): 0 unless the context's caster is the **left** hand; else `fCombatMagicDualCastChance` (372195, 0.33) × the caster's own cast chance (caster slot 08). For an attack spell that is lerp(0.05, 0.75, offensiveMult) released, lerp(0.25, 1, ...) streamed, so **1.65% to 24.75%** a try for a released attack spell, and 0.33 for a buff, heal, ward or cloak (their cast chance is 1). The combat style reaches it only through its offensive multiplier; there is no dual-cast field in `TESCombatStyle`.
- **The conditions** (PrepareDualCast): the right hand holds the **same spell**; magicka at least `fMagicDualCastingCostMult` (376265, 1.5) × cost + `fMagicDualCastingCostBase` (376262, 0), the cost through the perks (11321); both casters idle and ready (`bMLh_Ready`, `bMRh_Ready`) and the caster's own start check. A released spell waits for the right hand to be idle. A stream needs the right hand already channelling; after `fCombatMagicDualCastInterruptTime` (372192, 2.5 s) it is interrupted and the pair restarts as a dual cast. Distance plays no part.
- **The cast** (38762): same spell in both hands, spell type Spell, all casters idle; sets the dual flag (unless the spell has NoDualCastModifications) and casts from the left, at 1.5 times the cost. **The `CanDualCastSpell` perk entry is asked only for the player**, so an NPC needs no Dual Casting perk to dual-cast. What the Dual Casting perks do to an NPC's dual cast's power is not traced.
- **The equip side is the gate that matters:** the same spell must be in both hands, which the loadout (44899) decides. With "Varied AI choices" on, each hand's entry of a spell has its own random draw, so both hands hold the same spell less often than in vanilla, where the top spell tops both hands' lists, and the AI dual-casts less.

## Why the three behaviours

- **The same spell every time.** Category 0, best score first, deterministic score, no gate on a fire-and-forget attack spell but "not fleeing", and nothing that remembers the last cast.
- **Cloaks, eagerly.** Category 4 is offered a hand (and magicka) before category 0, and the cloak's gate is afford + not active + a range test that nearly every melee enemy passes (Flame Cloak, magnitude 10 and 60 s: R = 348 units). No timer and no magicka floor, where summons, disarm, paralysis and reanimation wait 30 s, wards want magicka above half, and heals want low health and a 15 s timer. When the cloak drops, the next rebuild puts it back in hand ahead of the attack spells. Its large score (magnitude x 60 s) plays no part: nothing outside category 4 is compared with it.
- **Never Fire Storm.** No registry entry for a hostile effect on a self-delivered spell: score -1 or 0, never queued (`dev/MAGIC.md`). With the Settings page's **Use self-targeting damage spells** on, we score them (below).

## Where a different chooser could go

Two separate things decide what a follower does, and they need different levers:

- **Which entry within a category** (which attack spell, which weapon). Scores decide it, and the score hook we already have reaches it.
- **Which kind of action** (cloak, ward or heal before attacking). The category order and the gates decide it; scores play no part.

The hook points:

1. **The score virtual (slot 0x0C), already hooked in `Pins.cpp`.** It runs once a second per entry and everything downstream reads its answer. Enough for a recency penalty and a weighted random pick within a category.
2. **The call to 44899 at 0x80f55d in 44858** (and 0x80f2fd at combat start). It is an existing call instruction, so `write_call` is valid. Run the original, then rewrite set B's pick, keeping its slot mask and items consistent. The place to replace the whole decision, category order included.
3. **Each caster's `CheckShouldEquip` (slot 0x0F).** Per-class vtable writes like the score hook, to add a cooldown to cloaks or a magicka floor to buffs.
4. **48118**, by a Detours entry hook. The last point before an equip, per slot and per tree tick.

There is no single argmax function to replace: the max is the queue pop inlined in 44899.

### Sampling, as a language model samples

The Gumbel-max trick: `argmax_i(log s_i / T + g_i)`, with each `g_i` drawn from Gumbel(0, 1), picks entry `i` with probability `s_i^(1/T) / sum_j s_j^(1/T)`. That is softmax over log scores, at temperature T.

So adding `T x g_i` to `log s_i` in the score hook, and returning `exp(...)`, turns the engine's own "best first" into a weighted random pick, with no change to 44899:
- **Scale-free:** multiplying every score by a constant changes nothing, so DPS against magnitude x seconds, or a follower at level 5 against one at 50, needs no normalising.
- **T = 1** picks in proportion to score. **T -> 0** is today's AI. **Large T** is nearly uniform.
- **A score of 0 or -1 stays out:** leave it alone, and it is still never queued. That keeps pins and bans intact. The one exception is ours: a hostile spell cast on oneself, which the engine scores 0 for want of a registry entry, is scored ("What we change").
- **A recency penalty** multiplies the score by a factor that recovers with time since the entry was last equipped or cast (`equippedItems` holds each equip time). The language-model analogue is a frequency or presence penalty.
- **Top-k or top-p:** zero every entry outside the best k, or outside the smallest set covering p of the probability, so a bad option is never picked by luck.

Two things to settle before building it:
- **When to draw.** A fresh `g_i` every rescore (once a second) would switch the pick as often as the minimum equip time allows (3 to 5 s), which reads as flicker. Draw once per entry and hold it until the held item changes or the entry is cast, so the follower commits and then reconsiders.
- **The other score readers.** The +0.1 held bonus, the 10% minimum-score rule and set A's range choice all read the same answer. Noise reaches them too. The range choice is the one that could change behaviour visibly: a sampled bow could pull a melee follower back to range.

The sampler, the penalty and their tests belong in `src/core` (a pure function from scores, a clock and a seed to adjusted scores); only the hook stays in `src/game`.

## What we change (built 2026-09-22, `wip-scoring`; not yet verified in play)

Weapons and attack spells only: category 0, a follower's (a player teammate, never the player, never an enemy). The Settings page's Combat section has the switch, **Varied AI choices**, on by default and saved with the game (`ft::Settings::variedAiChoices`); it gates the perks, the immunities and the variety, not the stand-down for a rule's cast, which is tactics' own and always on. Heals, wards and buffs keep the engine's gates and order. The code is `src/game/AiScore.cpp`, called from the score hook in `Pins.cpp` before the pins and bans. The testable part is `src/core/Variety.h` and `WaitsOnOwnCast` in `core/Evaluator.h`.

| | The engine | Ours |
|---|---|---|
| **Which attack spell** | Best score, every rescore: the same spell wins until magicka or range says otherwise | A random pick weighted by score: each spell's score is multiplied by a random factor `exp(T(g − γ))`, where `g` is a Gumbel draw, so the winner is spell `i` with probability ∝ `score_i^(1/T)`. `T` = 1, so a spell scoring twice another is cast twice as often |
| **Repeating** | Nothing remembers the last cast | Counted in attack casts, not seconds, so a quick caster and a slow one are treated alike: each of the last attack casts that was this spell takes its share, the last cast half the score, the one before a quarter, halving each cast back, each cast of it multiplying in (A, B, A leaves A x0.5 x0.875). The last eight are kept. A heal or a buff neither counts nor ages them. Forgotten when the fight ends |
| **The draw's reach** | – | The random factor is capped at ×10 either way: the Gumbel tail reached ×1152 in play, and the engine reads the same answer for the loadout's fighting range and the 10% rule on short-reach attacks. The cap moves the odds by about a point |
| **Swapping** | Only the engine's minimum equip time (3 s for magic) and a +0.1 bonus for the held item | Each entry's draw is held until something says it has had its turn or cannot have one: **its own** spell cast, and the engine's 3 s past; the entry unusable (a score of 0, an enemy it cannot touch); another enemy; the fight over. No timer of ours: until 2026-09-23 a draw with no cast went after 10 s, a guess. So a spell the draw favoured keeps its turn until used. Until the same day one cast redrew every entry: Serana's Ice Storm won a draw, was equipped, and lost the draw to the other hand's cast before it was ever cast |
| **Weapon damage** | Base × skill curve + MeleeDamage. No perks, no AttackDamageMult | × (our figure / the plain one). Our figure goes through the ModAttackDamage perk entry point against the actual enemy (Armsman, Overdraw, a perk against undead), plus AttackDamageMult |
| **Spell worth** | What one cast does (magnitude × duration): no cost, no cast time, so the biggest, dearest spell always wins | Per second of the follower's time, with magicka counted as time: `damage per cast / (charge + hold + price × cost)` (`core/AttackScore.h`). The price is 0 on a full pool and rises, squared, to one point's regeneration time on an empty one, so the dear spell wins while magicka lasts and the efficient one as it runs low. A stream (concentration spell) is its magnitude over the engine's 3 s scoring duration, and the cost per second over the same. A staff costs nothing (its charge is the recharge rule's business). A scroll costs no magicka but is the backup for when magicka runs low: its score is taken at the pool's drained share, squared (none full, all of it empty), where vanilla scores it as a spell with no Magic multiplier and never holds it back. Weapons are already per second, but the two are **not** one unit: the engine's per-cast figure for a spell carries its effect kinds' weights, the x2 for an aimed spell, the style's Magic multiplier and Script effects' AI scores, so a caster's spells ran 200 to 400 a second against a sword's 18 in play (Serana, 2026-09-22), as they outweigh it in vanilla too |
| **Spell damage** | The record's magnitude | × (the magnitude after the ModSpellMagnitude perks, against the enemy / the record's): Augmented Flames and the like |
| **Immunity** | Resistances only (`CheckResistance`). The attack-spell gate checks nothing else | Also zero when every hostile effect's conditions spare the enemy, asked as the engine asks when the effect lands: the ImmuneParalysis keyword, a drain that skips undead and automatons |
| **A hostile spell cast on oneself** | Fire Storm, Cold Fire Storm and their kind score 0 (no registry entry for hostile and self-delivered) and are never cast, known or not; and their entry's reach is 0 to 0, so the loadout would count them at a tenth anyway | A switch of its own, **Use self-targeting damage spells** (on by default, `ft::Settings::selfDamageSpells`), apart from Varied AI choices. Scored as the engine scores an aimed spell: the style's magic multiplier times each damage effect's magnitude x duration (at least 1 s) x the engine's weight for what it damages (health 1, magicka 0.5, stamina 0.33), less each enemy's own resistance, summed over every enemy the effect's area reaches at that enemy's distance (the combat group's targets, read under the group's lock; CommonLib's layout, not checked against the executable), so a storm among three bandits is worth three times one; a stagger, a slow and the like count for nothing, as in the engine's damage entry. None reach, and it stays 0. The entry's `maxRange` is set to the largest damage ring's radius, so the loadout's range test passes when the enemy is inside it and the optimal range (a point between min and max) is inside the rings. Areas are taken in feet at 64/3 units a foot, the engine's foot for a cloak's radius (34243); that an area takes the same foot is INFERRED. With Varied AI choices on too, then the same path as any attack spell: perks, immunities, per second, recency and the draw; with it off, that engine-like figure is the answer. No friendly-fire check: a player who does not want them cast bans them. Where the engine's equip check refuses it -- the spell is filed under the caster of its best-matching effect, for Cold Fire Storm probably the script caster, whose check (45448) asks things made for another kind of spell -- ours allows it: the switch on, a follower's attack spell cast on oneself with damage rings, last scored above 0, affordable, the caster not fleeing (`SelfDamageMayEquip`, through the slot 0x0F replaced beside the score). The log says each refusal allowed, and the entry line names each entry's caster class. **Seen in play (2026-09-23):** scored top of Serana's attack list, and not equipped, the reach 0 cutting it to a tenth; then, with the rest banned, top at 5220 and absent from every loadout, even the one at any range, while she fought with Drain Life and then her fists: the equip check, INFERRED, answered by the override (not yet seen); with the reach set and the stagger no longer counted as damage, it lost on worth against one cave bear (370 a second against Cold Devastation's 2000 to 2500), which is what counting the crowd answers. **Not yet seen:** a cast (its caster class comes from its best registry effect, for Cold Fire Storm probably a script one) |
| **A buff that would dispel a running one** | Casts it: its gate asks only whether *this* spell is running (45344), so two cloaks (both `MagicCloak`, both "dispel with keywords") put each other out in turn. Serana cast Cold Flame Cloak and Blood Aura about ten times in 90 s, magicka 154 to 19 | Scores 0 while an effect it would dispel runs on the caster, so whichever is up stays up until it expires. **Always on, every actor** (`src/fix/DispelHold.cpp`): it fixes the engine, and a modlist that hands an enemy two cloaks thrashes the same way |
| **A rule's cast** | The AI keeps casting its own spells. A rule waits for each cast to end and can wait a whole fight, or arm and time out while the AI starts another | While a rule waits on the follower's own cast, or holds a cast record, their own spells score 0 (the spell our record casts excepted). The AI finishes the cast in hand, starts no other, and the rule gets its turn |

**What the log says** (`[ai]`, at debug):
- **Once per entry per draw,** every component of its score. For a spell: the engine's per-cast figure, the perk factor, the charge and hold (or the stream's length), the cost and magicka's price with the pool and its regeneration, the per-second result, the recency factor, the draw, the answer and the draw's number. For a weapon: the engine's per-second figure, the perk factor and the enemy.
- **At every cast,** the spell cast, their magicka, and every attack entry's latest answer, highest first: what the choice was made against.
- A stand-down, and a spell answered 0 because every hostile effect spares the enemy, once each per fight.
- **Once per entry per fight, every entry,** switch on or off: its category, the engine's score, and its reach (minimum, optimal, maximum, equip range), which the loadout scores down by ×0.1 outside.
- **The loadout, each time it changes:** the set the engine equips from, with its items' answers and the set's score; the set it would want at any range; what is in hand; and the distance readings the range test uses. Read at the first entry of a rescore, on the AI's own thread. The set's fields are read by offset: CommonLib names them wrongly (`dev/COMMONLIB.md`).

**Why it is better:**
- **Variety without flicker.** The pick is random but weighted, so strong spells still dominate. The draw is scale-free, so it needs no tuning per follower or level. Holding the draw until a cast keeps the engine's own "best first" from turning it into swapping.
- **A truer estimate.** The score now counts what the follower's perks and effects do to this enemy, which the engine left out, and stops wasting casts on an enemy the spell cannot touch.
- **Tactics and AI stop stepping on each other.** Tactics already waited for the AI rather than interrupting it (`core/Evaluator.cpp`, CastAvailability). Now the AI waits for tactics, for as long as a rule is actually waiting.

A scroll is a spell record and takes the spell path whole. A staff is a weapon record whose cast is its enchantment, and is scored as that enchantment: perks, conditions, draw and penalty. Whether the cast event names the enchantment for a staff, which the penalty needs, is not yet seen.

**Left as the engine has it, on purpose:**
- **No random draw on weapons.** A random swap between a sword and a bow is worse than none. Weapons do still compete with the varied spells, so a spellsword sometimes casts where they would have swung.
- **Tempering and a weapon's enchantment.** The engine's entry names the form, not the copy.
- **The category order and every gate.** Cloaks are still eager; section 4's hook points are where that would change.

## Combat styles: the AI's tuning

A `CSTY` record is the combat AI's tuning, not its logic: the engine code above reads its numbers and flags. An actor uses the style on their base record (`TESNPC::combatStyle`), and in a fight their `CombatController` holds a pointer to it too. The field descriptions are the Creation Kit wiki's ("Combat Style"), kept here because that page refuses automated fetches. The ranges are measured: every style in this load order, 163 of them, read through houseCARL (2026-09-03).

**Where the code above reads it:**
- The **equipment score multipliers** multiply each entry's score at every rescore, once a second (section 2), so a change takes effect within a second, not at the next fight: Magic, Shout and Staff on magic entries (45084), Melee, Ranged and Unarmed on weapons (26416).
- The **offensive multiplier** sets the AI's hold before releasing an attack spell (45354) and its cast chance, and through that the dual-cast chance (section 6).
- The **defensive multiplier** sets the health a heal waits for (section 4, Restore).
- The **Allow Dual Wielding** flag: an actor whose style forbids it takes a left-hand weapon straight off again.

**Two scales:**

| family | fields | range seen | neutral |
|---|---|---|---|
| chances and movement | offensive, defensive, group offensive, avoid threat, special attack, circle, fallback, flank distance, stalk time, strafe | 0 to 1 | -- |
| score and attack multipliers | the six equipment scores; attack staggered, power attack staggered, power attack blocking, bash, bash recoiled, bash attacking, bash power attacking | 0 to 10 | 1 |

The panel shows each value as `x / 1` or `x / 10` accordingly. Reference points: csHumanMagic (Marcurio) has magic score 4.05, melee 0.76, offensive 0.65, defensive 0.5, Dueling; csHumanMissile has ranged 3.2, melee 0.83. Of the 163 styles, 113 are Dueling, 27 Dueling with dual wielding, 11 Flanking, 2 Flanking with dual wielding.

**The fields (Creation Kit wiki):**

- General
  - **Offensive Mult**: works with Defensive. The higher, the more likely a character attacks, the more often, and the more often with a power attack.
  - **Defensive Mult**: the higher, the more a character blocks, the longer the block is held, and the more they bash if they can.
  - **Group Offensive Mult**: overrides Offensive in a group: the more actors attacking one target, the less offensive each is, by this mult. Higher keeps them offensive in groups.
  - **Avoid Threat Chance**: not used, or use unknown to the wiki's author.
  - **Equipment Score Mults**: the higher, the more likely the actor uses that kind of equipment. Multiplied into the damage output of the attack, so a weak melee attack against strong spells needs a very high melee mult before the actor prefers melee: a comparison of weighted damage, not a share of the time (section 2 has the formulas).
- Melee
  - **Attack Staggered** / **Power Attack Staggered**: the higher, the more likely an attack, or a power attack, on a staggered target.
  - **Power Attack Blocking**: the more likely a power attack on a blocking target, to break the block.
  - **Special Attack**: not used, or use unknown.
  - **Bash**: the more likely a bash (a shield's, or an attack flagged as a bash), which can stagger and interrupt. **Bash Recoiled**, **Bash Attack**, **Bash Power Attack**: against a target recoiling from its own blocked attack, mid-attack, mid-power-attack.
  - **Allow Dual Wielding** (flag): lets an NPC dual wield; works only on NPCs with dual-wielding animations, humanoids.
- Close range, one of two modes by flag
  - **Dueling**: **Circle Mult** (how much the actor circles the target rather than standing still), **Fallback Mult** (chance to back off).
  - **Flanking**: **Flank Distance** (distance kept while flanking), **Stalk Time** (time spent flanking before attacking).
- Long range: **Strafe Mult**, how much the actor strafes to dodge projectiles out of melee range.
- Flight: dragons only; not shown.

**What the panel does with it:**
- The Combat Style tab, before Tactics, shows the live style with these descriptions as hover text and only the active close-range pair; the two unused fields and the flight fields are left off.
- A left-hand weapon pin gives the follower a **runtime copy** of the style with dual wielding allowed (`AllowDualWield` in `Tactics.cpp`), assigned to their record and their live controller. Vanilla styles are shared by every actor of a kind (csHumanMagic by every mage), so a style is never edited in place. The copy (`CreateDuplicateForm`, a 0xFF FormID) is not written to the save: created forms are saved only for weapons, armour, potions, enchantments and references, and the NPC change form does not carry the combat style. So it, and anything pointing at it, is gone on reload, which is what makes it safe to uninstall over.
- Next, perhaps: a dropdown of named styles (wizard, spellsword, berserker, archer) as tuned copies, the `SetCombatStyle` palette `PLAN.md` 3.8 anticipates. Since the score multipliers are read at every rescore, such a change would take effect within a second.

The style used to be this project's only lever on the AI, the decision logic being out of reach. It is no longer: sections 2 to 5 are the logic, and "What we change" hooks it.

## Not yet read or verified

- Anything here, in play: a debug log of the hook's raw and adjusted scores per fight would check sections 2 and 3 at once.
- The sort routine 44914 and the per-category queue order (44921).
- What sets `item+0x38`.
- 47305 (the caster test) and 47332's branches that are not projectiles.
- The distance helpers feeding +0x1A8 to +0x1C0.
- The behaviour tree's calls into the cast checks.
- Which of categories 2 and 3 holds the long ward.
- GMST overrides in Skyrim.esm or an ini.
- The SE (1.5.97) IDs for all of it (`dev/VERSIONS.md`).
