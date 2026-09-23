# How the combat AI chooses what to hold

Read from the disassembly of 1.6.1170 (AE Address Library IDs) on 2026-09-22, branch `wip-scoring`. **Nothing here is verified in play yet.** Each claim is READ (seen in the instructions) or INFERRED; unmarked means READ. GMST values are the executable's compiled defaults; whether Skyrim.esm or an ini overrides any of them is not checked. The working notes, with raw dumps and helper scripts, were in `build/scoring/` (untracked): `score-functions.md`, `selection.md`, `casters.md`.

It answers three questions: why a follower always casts the same spell, why cloaks are cast so eagerly, and why Fire Storm is never cast. And it names where a different chooser (a recency penalty, a weighted random pick) could go in.

## The short version

- **Nothing on the choosing path is random.** The engine's RNG (68276, seeded by 68275, 266 callers) is not called from the inventory code (44836-45100), the equip context (48117-48121), or their direct callees. Randomness in combat decides *when* to act (cast chance, dual-cast chance 49095), not *what* to hold.
- **It is not one argmax over everything.** Once a second the inventory is rescored and a loadout built by walking the categories in a **fixed order**, taking the best-scoring entry of each that still fits a free hand. Categories are never compared with each other by score; a score only competes inside its own category.
- **Buffs get a hand before attacks do.** Restore, wards and long buffs (cloaks among them) are offered a hand ahead of offence. Their gates, not their scores, decide whether they take it.
- **Inside offence, the same thing wins every time.** Melee, bows and attack spells are all category 0 and compete on score; the score is deterministic and has no memory of what was used, so the top entry stays on top until magicka, range or a gate says otherwise.

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

## Why the three behaviours

- **The same spell every time.** Category 0, best score first, deterministic score, no gate on a fire-and-forget attack spell but "not fleeing", and nothing that remembers the last cast.
- **Cloaks, eagerly.** Category 4 is offered a hand (and magicka) before category 0, and the cloak's gate is afford + not active + a range test that nearly every melee enemy passes (Flame Cloak, magnitude 10 and 60 s: R = 348 units). No timer and no magicka floor, where summons, disarm, paralysis and reanimation wait 30 s, wards want magicka above half, and heals want low health and a 15 s timer. When the cloak drops, the next rebuild puts it back in hand ahead of the attack spells. Its large score (magnitude x 60 s) plays no part: nothing outside category 4 is compared with it.
- **Never Fire Storm.** No registry entry for a hostile effect on a self-delivered spell: score -1 or 0, never queued (`dev/MAGIC.md`).

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
- **A score of 0 or -1 stays out:** leave it alone, and it is still never queued. That keeps pins, bans and Fire Storm's exclusion intact.
- **A recency penalty** multiplies the score by a factor that recovers with time since the entry was last equipped or cast (`equippedItems` holds each equip time). The language-model analogue is a frequency or presence penalty.
- **Top-k or top-p:** zero every entry outside the best k, or outside the smallest set covering p of the probability, so a bad option is never picked by luck.

Two things to settle before building it:
- **When to draw.** A fresh `g_i` every rescore (once a second) would switch the pick as often as the minimum equip time allows (3 to 5 s), which reads as flicker. Draw once per entry and hold it until the held item changes or the entry is cast, so the follower commits and then reconsiders.
- **The other score readers.** The +0.1 held bonus, the 10% minimum-score rule and set A's range choice all read the same answer. Noise reaches them too. The range choice is the one that could change behaviour visibly: a sampled bow could pull a melee follower back to range.

The sampler, the penalty and their tests belong in `src/core` (a pure function from scores, a clock and a seed to adjusted scores); only the hook stays in `src/game`.

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
