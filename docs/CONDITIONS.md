# Conditions: Status, Armor, Resistance, Attacked By, Lowest/Highest, Summon, Corpse

Design for the rule conditions built 2026-09-04 from research done the
same day (sources at the end). All of it is built: Status, Armor with
Lowest and Highest, Resistance, Attacked by with the Attacker target,
Health lowest and highest, and the other followers as named subjects. The
signals are read off the engine on the 500 ms tick, into the RE-free
`Snapshot`, and the evaluator only ever compares what the snapshot
carries. Section 8 is what remains to be seen in play.

## 1. The shape of a rule

Today a condition is `subject + predicate + one number`. Three of the new
conditions take a *kind* as well as, or instead of, a number:

| Condition | Kind | Number |
|---|---|---|
| Status | poisoned, burning, frostbitten, shocked, paralysed, staggered, fleeing, bleeding out, invisible, ethereal, blocking, casting, sneaking | none |
| Resistance | fire, frost, shock, magic, poison | a percent, below or above; or lowest / highest |
| Attacked by | any; melee, ranged, magic; fire, frost, shock, poison | none |
| Armor | none | a percent, below or above; or lowest / highest |
| Health / Stamina / Magicka | lowest, highest | or the existing below / above % |

Every measure -- health, stamina, magicka, armour, a resistance -- is asked the same way: a percent below or above, or a group's lowest or highest of it (changed 2026-09-04 from named bands; the bands below are kept as reference values).

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

Damage reduction = min(fMaxArmorRating 80%, rating x fArmorScalingFactor 0.12 / 100 + pieces x fArmorBaseFactor 0.03). The cap is reached at a displayed 567 with four pieces. `kDamageResist` is the displayed figure, worn armour with tempering and skill included. Since 2026-09-04 the two inputs come from the engine's own accessors, `Actor::CalcArmorRating()` and `Actor::GetArmorBaseFactorSum()`, rather than a recount of the worn slots; only the combination is ours, because the engine does it inline in the damage code. A mod that changes the settings or the ratings is reflected; one that hooks the formula itself (Armor Rating Rescaled, Armor Rating Redux) is not. The snapshot carries the reduction as a fraction, not the displayed number, so the condition means the same thing on a bandit in fur and a chief in plate.

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

**The condition:** the reduction as a percent, below or above 25 / 50 / 75%, or a group's Lowest and Highest, as Health's. (The named bands Low / Medium / High, at 25% and 55%, were the first version and are gone.)

## 4. Resistance

`kResistFire` 41, `kResistShock` 42, `kResistFrost` 43, `kResistMagic` 44,
`kResistDisease` 45, `kPoisonResist` 40. Weakness is a negative value. NPCs
are uncapped: 100 is immunity. Vanilla values cluster at 25, 33, 50 and
100 (Nord frost 50, Dunmer fire 50, Breton magic 25, draugr frost 50 and
poison 100, atronachs 100 to their own element and -33 to the opposite,
dragons 50 own and -25 opposite, vampires frost 50 and fire -50).

**The condition:** the value as a percent, below or above 25 / 50 / 75%, or a group's Lowest and Highest of that kind: "Resistance Fire > 75%" is the atronach, "Resistance Frost < 25%" catches the weakness too. (The named bands Weak / Normal / High / Immune were the first version and are gone.) The snapshot carries the values per actor view.

## 5. Attacked by

An actor is "attacked by fire" when something has hit them with fire in the
last few seconds, or fire is running on them now. Three sources, merged
into one per-actor table of `kind -> (game time, attacker)`:

1. A sink on `TESHitEvent` (target, cause, source form, projectile, flags).
   It fires once for a physical hit and once per magic effect. The source
   form says the kind: a weapon or nothing is a blow -- **melee**, or
   **ranged** when the event names a projectile, an arrow or a bolt; a
   magic item is **magic**, and its effects bucket by `resistVariable` and
   `IsPoison()` as well, so a fire hit is magic and fire both. "Attacked by ranged" is the archer in particular, "attacked by melee" the one at the follower's face, "attacked by magic" any caster (2026-09-04, for the Target action in docs/ACTIONS.md 6 and for armour buffs against blows).
2. A sink on `TESMagicEffectApplyEvent` for effects that skip the hit event
   (`kNoHitEvent`): cloaks, hazards, spit. Which ones do is to be tested.
3. The active-effect scan of section 2, with the effect's `caster` as the
   attacker, for anything still running.

The window is 3 s. "Attacked by" is true within it; the attacker is the
most recent. The snapshot carries, per actor view, the kinds seen in the
window and the attacker's id, and the rule can aim its action at the
attacker: `Attacker` on the Then side, beside Self, Player, Ally, Enemy,
Target and a named follower. That is what "hit the enemy who is doing the attacking" needs.

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

## 6a. Summon and Corpse (built 2026-09-05, not yet played)

**Summon: None / Active** asks whether an actor commands a summon or a raised corpse right now. The engine keeps a per-actor list of commanded actors on the middle-high process (`commandedActors`, one entry per summon or reanimated corpse with the active effect that made it), and the sensor counts it into the actor's traits, so the question is answerable about the follower, the player, an ally, a named follower or an enemy. The two sit under one "Summon" heading in the Condition cascade. The typical pair: `Self: Summon none -> Self: Cast Conjure Flame Atronach`, above a rule that does something else while the atronach is out.

**Corpse** is a group subject beside Ally and Enemy, with its own three questions and no other: **None**, **Highest level**, **Lowest level**. The sensor walks the loaded actors for the dead within 3000 units, skipping any already commanded (a raised corpse is someone's) and any carrying `MagicNoReanimate` (06F6FB), which is the Reanimate archetype's one condition. Each corpse carries its level. Highest and Lowest bind the corpse, so the Then side offers **Corpse** as a target and `Corpse: Highest level -> Corpse: Cast Reanimate Corpse` aims the cast at it through the existing target input. None binds the follower, for `Corpse: None -> Self: Cast Conjure ...`, and the Corpse target is not offered under it.

**The rat problem.** Reanimate spells raise corpses up to a level: the effect's magnitude, 6 for Raise Zombie, 13 for Reanimate Corpse, 21 for Revenant, 30 for Dread Zombie, read off each known spell into the snapshot as a cap. The Corpse subject measures the dead against the cap of the rule's own cast spell, so Highest level is the highest the spell can actually raise, and a rule whose spell has no cap (a conjuration) sees every corpse. Serana raises the rat because vanilla picks whatever is nearest; this picks the strongest the spell will take.

**Only a Reanimate goes at a corpse.** Reanimation is a property of the magic effect, not a name: the effect record's Archetype is `Reanimate` (22), the value the engine raises a corpse by, so a mod's reanimate spell has it whatever it is called. The Cast menu under the Corpse target lists only spells with such an effect, and the evaluator refuses any other cast aimed at a corpse as unsupported. The cap is read off the same effect.

**A Location spell** -- a conjuration -- is offered under every target and, aimed at Self, goes at the follower's own feet rather than the enemy's; every other aimed spell aimed at no one still goes at the enemy.

**Not yet seen in play:** the UseMagic package aiming at a dead actor (every target so far has been alive), the engine's own level check agreeing with ours, and a raised corpse counting in `commandedActors` for the follower rather than for nobody.

**The Summons tab**, after Magic, shows what the follower commands: a chip per summon carrying its name, then health, stamina and magicka bars, level, whether summoned or raised, seconds remaining on the commanding effect, the reference and base FormIDs, and the same sheet the Character tab builds for the follower.

## 7. What was built, where

- `core/Kinds.h`: StatusKind, ArmorBand and BandOf, DamageKind, ResistBand.
- `core/Snapshot.h`: ActorTraits -- status bits, armour reduction, six
  resistances, attacked-by bits and the attacker -- on the follower, the
  player, each ally and each enemy.
- `core/Rule.h`: predicates Status, Armor, Resistance, AttackedBy,
  HealthLowest/Highest, ArmorLowest/Highest; the rule's statusKind and
  damageKind; SubjectKind::Follower with subjectForm; the Attacker target.
- `game/Sensors.cpp`: ReadTraits and DamageReduction; the party and enemy
  walk. `game/Hits.cpp`: the hit table and its two sinks.
- `tests/test_evaluator.cpp`: one case per condition.

## 8. To test in play

- `kParalyzed` against the archetype; how long `staggered` stays set; what
  `magicCasters[]` shows during a concentration spell.
- Whether the hit event fires for cloaks, hazards and concentration ticks,
  and how often.
- The walk's list against the player's combat group `targets`, once, in
  the log; and how soon a dead or fled enemy drops out.
- The `armor ...:` line the sensors log once per actor: that
  `GetArmorBaseFactorSum` reads as pieces x 0.03 (0.06 for robes and boots)
  and `CalcArmorRating` as the displayed rating, so the reduction in the
  sheet's Armor row matches the 6% / 10% seen with the old slot count. And
  the logged values for a fight's enemies, to replace the estimated tiers.

## Sources

UESP Armor, Resist Magic, Followers; CK wiki Actor Value, OnHit, List of
Animation Variables; CS wiki fArmorRatingMax; Armor Rating Rescaled and
True Armour Rating (Nexus); alandtse/CommonLibSSE-NG `Actor.cpp`,
`MagicTarget.cpp`; the CharmedBaryon 3.7.0 headers in this build tree;
Skyrim.esm records read through houseCARL (the 112 damage-keyworded
effects, the `AbResist*` abilities, the armour settings and base ratings).
