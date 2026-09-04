# Conditions: Status, Armor, Resistance, Attacked By, Lowest/Highest

Design for the next set of rule conditions, from research done 2026-09-04
(sources at the end). Nothing here is built yet. The signals are read off
the engine on the 500 ms tick, into the RE-free `Snapshot`, and the
evaluator only ever compares what the snapshot carries.

## 1. The shape of a rule

Today a condition is `subject + predicate + one number`. Three of the new
conditions take a *kind* as well as, or instead of, a number:

| Condition | Kind | Number |
|---|---|---|
| Status | poisoned, burning, frostbitten, shocked, diseased, paralysed, staggered, fleeing, bleeding out, invisible, ethereal, blocking, casting, sneaking | none |
| Resistance | fire, frost, shock, magic, poison, disease | a band |
| Attacked by | physical, magic, fire, frost, shock, poison | none |
| Armor | none | a band |
| Health / Stamina / Magicka | lowest, highest | or the existing below / above % |

So `Rule` gains one field, `conditionKind`, an enum on the wire like
everything else (`"poisoned"`, `"fire"`), read only by the predicates that
take one. One predicate per condition, not one per kind: `status` with
`kind: poisoned`, not `status-poisoned`, so a profile lists fourteen
statuses under one name and a new status is an enum value, not a predicate.

Bands are predicates' numbers, so the wire stays a float: `armor-at-least`
with 0.55 is "High". The menu offers the band names; the argument text
shows them.

## 2. Status: what each one reads

All from the actor, per tick. "Effect" means a walk of
`MagicTarget::GetActiveEffectList()` skipping `kInactive`/`kDispelled`.

| Status | Signal | Confidence |
|---|---|---|
| burning | an effect whose base effect's `resistVariable` is `kResistFire` (keyword `MagicDamageFire` 0x01CEAD as the second test) | high |
| frostbitten | same with `kResistFrost` / `MagicDamageFrost` 0x01CEAE | medium: fire-and-forget frost bolts are `NoDuration` and leave no effect; the hit table (section 5) backs it |
| shocked | same with `kResistShock` / `MagicDamageShock` 0x01CEAF | medium, same caveat |
| poisoned | an effect whose `spell->IsPoison()` | high |
| diseased | an effect whose spell type is `kDisease` | high |
| paralysed | `boolBits kParalyzed`, or an effect with archetype `kParalysis` | medium: which flips first is to be seen |
| staggered | `actorState2.staggered` | medium |
| fleeing | `combatController->IsFleeing()`, null-checked | high |
| bleeding out | `ActorState::IsBleedingOut()` | high, already read |
| invisible | archetype `kInvisibility` or `kInvisibility` value above 0 | high |
| ethereal | archetype `kEtherealize` | high |
| blocking | `Actor::IsBlocking()` | high |
| casting | a `magicCasters[]` slot in `kCharging`/`kCasting` | medium |
| sneaking | `Actor::IsSneaking()` | high, already read |

The vanilla condition functions (`HasMagicEffectKeyword`, `IsStaggered`,
`GetAttacked`...) can be built at runtime as a `TESConditionItem`, but the
direct reads above are what they read, so they are kept as a debugging
cross-check, not the hot path.

Snapshot: a `status` bit set per actor view.

## 3. Armor

Damage reduction = min(80%, (displayed + 25 per worn piece) x 0.12). The
cap is reached at a displayed 567 with four pieces. `kDamageResist` is the
displayed figure, worn armour with tempering and skill included; the
runtime's `armorRating` and `armorBaseFactorSum` give the reduction
exactly, and that is what the snapshot carries: the follower's reduction as
a fraction, not the displayed number, so the bands mean the same thing on
a bandit in fur and a chief in plate.

Creature skins all rate 0; a dragon or a giant reads as Low, which is what
a rule about armour should say about them.

Estimated tiers (to be replaced by logged values):

| Displayed | Reduction | Who |
|---|---|---|
| 0-20 | 0-5% | mages, dragon priests, animals, atronachs, dragons, giants, unarmoured draugr |
| 50-90 | 15-25% | bandits, Forsworn, low draugr, Falmer |
| 90-160 | 25-40% | guards, soldiers, bandit outlaws |
| 150-300 | 40-60% | chiefs, deathlords, Thalmor |
| 300-567 | 60-80% | late named enemies, the player, geared followers |

**Bands:** Low under 25%, Medium 25% to 55%, High 55% and up. Lowest and
Highest bind the group's extreme, as Health's do.

## 4. Resistance

`kResistFire` 41, `kResistShock` 42, `kResistFrost` 43, `kResistMagic` 44,
`kResistDisease` 45, `kPoisonResist` 40. Weakness is a negative value. NPCs
are uncapped: 100 is immunity. Vanilla values cluster at 25, 33, 50 and
100 (Nord frost 50, Dunmer fire 50, Breton magic 25, draugr frost 50 and
poison 100, atronachs 100 to their own element and -33 to the opposite,
dragons 50 own and -25 opposite, vampires frost 50 and fire -50).

**Bands:** Weak below 0, Normal 0-24, High 50 and up, Immune 100 and up.
The snapshot carries the six values per actor view.

## 5. Attacked by

An actor is "attacked by fire" when something has hit them with fire in the
last few seconds, or fire is running on them now. Three sources, merged
into one per-actor table of `kind -> (game time, attacker)`:

1. A sink on `TESHitEvent` (target, cause, source form, projectile, flags).
   It fires once for a physical hit and once per magic effect. The source
   form says the kind: a weapon or nothing is physical; a magic item's
   effects bucket by `resistVariable` and `IsPoison()`.
2. A sink on `TESMagicEffectApplyEvent` for effects that skip the hit event
   (`kNoHitEvent`): cloaks, hazards, spit. Which ones do is to be tested.
3. The active-effect scan of section 2, with the effect's `caster` as the
   attacker, for anything still running.

The window is 3 s. "Attacked by" is true within it; the attacker is the
most recent. The snapshot carries, per actor view, the kinds seen in the
window and the attacker's id, and the rule can aim its action at the
attacker: a new action target, `Attacker`, alongside Self, Player and
Target. That is what "hit the enemy who is doing the attacking" needs.

## 6. Sensing the party and the enemies

By definition, not by the engine's combat group:

- **Ally**, for a follower, is the player and every other actor with the
  player-teammate flag -- the player's other followers -- alive and loaded.
- **Enemy** is anyone the compass paints red for the player: an actor in
  combat and hostile to the player, alive and loaded.

Both come from one walk of the loaded high actors per tick
(`ProcessLists::ForEachHighActor`), testing `IsPlayerTeammate()`,
`IsInCombat()` and `IsHostileToActor(player)`. At a few dozen loaded actors
and 500 ms that is well inside the budget. The player's own combat group
(`targets`) would say the same thing more cheaply and is the thing to
compare against once, in the log; the walk is what the definition says.

This is what makes the Ally and Enemy subjects real, and it is what a
named-follower subject needs: `SubjectKind::Follower` with the actor's
form, listed by name in the menu after the player.

## 7. Order of work

1. Group sensing, since Ally, Enemy, Lowest/Highest and named followers
   all wait on it. One tick, one log line of who is who.
2. Per-actor view: status bits, armour reduction, six resistances. Shared
   by Self, Player, Ally, Enemy views.
3. The hit table and the two sinks; Attacked by; the Attacker target.
4. The menu, with kinds and bands; wire names; tests for each predicate.

## 8. To test in play

- `kParalyzed` against the archetype; how long `staggered` stays set; what
  `magicCasters[]` shows during a concentration spell.
- Whether the hit event fires for cloaks, hazards and concentration ticks,
  and how often.
- The walk's list against the player's combat group `targets`, once, in
  the log; and how soon a dead or fled enemy drops out.
- Logged `kDamageResist`, `armorRating` and `armorBaseFactorSum` for a
  fight's enemies, to replace the estimated tiers.

## Sources

UESP Armor, Resist Magic, Followers; CK wiki Actor Value, OnHit, List of
Animation Variables; CS wiki fArmorRatingMax; Armor Rating Rescaled and
True Armour Rating (Nexus); alandtse/CommonLibSSE-NG `Actor.cpp`,
`MagicTarget.cpp`; the CharmedBaryon 3.7.0 headers in this build tree;
Skyrim.esm records read through houseCARL (the 112 damage-keyworded
effects, the `AbResist*` abilities, the armour settings and base ratings).
