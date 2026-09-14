# Conditions: Status, Armor, Resistance, Attacked By, Lowest/Highest, Summon, Corpse, Weapon

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
| Type | man: Breton, Imperial, Nord, Redguard; elf: Dark Elf, Falmer, High Elf, Snow Elf, Wood Elf; beast: Argonian, Khajiit, Orc; creature: Animal, Automaton, Daedra, Dragon, Giant, Spriggan, Troll, Undead, Vampire, Werewolf; or any of a group (see 2a) | none |
| Status | poisoned, burning, frostbitten, shocked, paralyzed, staggered, fleeing, bleeding out, invisible, ethereal, blocking, casting, sneaking (not all about everyone, see 9) | none |
| Resistance | fire, frost, shock, magic, poison | a percent, below or above (0 included, see 4); or lowest / highest |
| Hit by | any; melee, ranged, magic; fire, frost, shock, poison | none |
| Hit type | melee, ranged, magic; fire, frost, shock, poison -- no "any", see 9 | none |
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

## 2a. Type: what kind of being (built 2026-09-12)

Four groups in the menu, alphabetical, each opening on Any (the group) and its members by name. A rule names one kind; the snapshot carries a bit per kind, and a group's Any is any member's bit or the group's own. Asked of an enemy or an ally only, the subjects that are a group: the follower themself, the player and a named follower are each one being, and a rule about what they are would be true always or never.

The engine has two layers, both on the race record (read with houseCARL, 2026-09-12). The `ActorType` keywords are the coarse class its own conditions use: the sun spells and Turn Undead gate on `HasKeyword ActorTypeUndead` and nothing else. The race is the fine class. A vampire is a third thing: a vampire Nord is `NordRaceVampire`, named "Nord", with the NPC keyword plus `Vampire` and `Undead`.

| kind | read from | notes |
|---|---|---|
| the peoples: Breton, Imperial, Nord, Redguard, Dark Elf, High Elf, Wood Elf, Snow Elf, Argonian, Khajiit, Orc | the race record's editor id contains the vanilla name (`NordRace`), which the vampire, child and DLC variants do (`NordRaceVampire`, `DLC1NordRace`) | a vampire Nord is a Nord. A ghost is not: a ghost carries `ActorTypeGhost` on the base over a living race, and answers to Undead alone |
| Man (Any) | any of its four, or the Elder race | |
| Falmer | race id contains `Falmer` | `ActorTypeCreature` to the engine, no elf keyword; Wuuthrad's "Elf Slayer" perk is `GetIsRace` over the three elven races and Falmer, so it is an Elf here and a Creature both |
| Animal, Daedra, Dragon, Troll, Undead, Vampire | the keyword of that name on the actor (`ActorTypeAnimal`, `ActorTypeDaedra`, `ActorTypeDragon`, `ActorTypeTroll`, `ActorTypeUndead`, `Vampire`) | trolls are Animal and Troll; death hounds Undead, not Animal; dremora Daedra and NPC; atronachs Daedra and Creature |
| Automaton | `ActorTypeDwarven`, less `DLC2AshSpawnKeyword` | Ash Spawn carry the Dwarven keyword, an oddity of Dragonborn |
| Giant | `ActorTypeGiant` on the actor, or race id contains `Giant` and not `Lurker` | the Lurker's race is named Giant |
| Spriggan | race id contains `Spriggan` | no keyword of its own |
| Werewolf | race id contains `WerewolfBeast` or `WerebearBeast` | no keyword of its own |
| Creature (Any) | `ActorTypeCreature`, or any member | hagravens, wisps, ice wraiths, gargoyles, rieklings answer to this and nothing finer |

Left out: Ghost folds into Undead; hagraven, wisp, ice wraith, gargoyle, riekling and chaurus are single races and one line each to add. Bound weapons are not actors; there is no keyword for a quest item.

## 3. Armor

Damage reduction = min(fMaxArmorRating 80%, rating x fArmorScalingFactor 0.12 / 100 + pieces x fArmorBaseFactor 0.03). The cap is reached at a displayed 567 with four pieces. The rating is the `kDamageResist` actor value, the displayed figure: the worn armour with tempering, skill and perks, plus every effect running on the value, a Fortify Armor Rating enchantment, a potion, a flesh spell. Not `Actor::CalcArmorRating()`, which is the pieces alone and was the input from 2026-09-04 to 2026-09-11: Frea in Nordic Carved with a +100 Fortify Armor Rating helmet read 392.5 on the value and 292.5 there, and the sheet listed the enchantment in its tooltip against a total without it. The value is written when the pieces change and can trail a skill gained since, which is accepted. The hidden bonus is `Actor::GetArmorBaseFactorSum()`. Only the combination is ours, because the engine does it inline in the damage code. A mod that changes the settings or the ratings is reflected; one that hooks the formula itself (Armor Rating Rescaled, Armor Rating Redux) is not. The snapshot carries the reduction as a fraction, not the displayed number, so the condition means the same thing on a bandit in fur and a chief in plate.

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

**The condition:** the value as a percent, below or above 0 / 25 / 50 / 75%, or a group's Lowest and Highest of that kind: "Resistance Fire > 75%" is the atronach, "Resistance Frost < 25%" catches the weakness too. (The named bands Weak / Normal / High / Immune were the first version and are gone.) The snapshot carries the values per actor view.

**Zero is offered here and nowhere else (2026-09-09).** A resistance is the one measure in the panel that commonly goes negative -- a Dunmer's -25 to frost, a vampire's -50 to fire, an atronach's -33 to the opposite element, and whatever a curse or a cloak has put on -- so "< 0%" is "weak to this" and "> 0%" is "resists it at all", two questions no threshold above zero can express. Health, magicka, stamina and armour never go below zero, so they start at 25% and 0 would be a dead entry.

**Melee, Ranged and Any are not offered under Resistance** and are refused if a hand-written profile asks: nothing resists a blow or an arrow but armour, which is its own condition, and nothing resists "any". `IsDamageKindValidFor` in `core/Rule.cpp` says so, the menu is built from it, and the evaluator reports InvalidCondition rather than answering against the 0 those slots hold.

## 5. Attacked by

An actor is "attacked by fire" when something has hit them with fire in the
last few seconds, or fire is running on them now. Three sources, merged
into one per-actor table of `kind -> (game time, attacker)`:

1. A sink on `TESHitEvent` (target, cause, source form, projectile, flags).
   It fires once for a physical hit and once per magic effect. The source
   form says the kind: a weapon or nothing is a blow -- **melee**, or
   **ranged** when the event names a projectile, an arrow or a bolt; a
   magic item is **magic**, and its effects bucket by `resistVariable` and
   `IsPoison()` as well, so a fire hit is magic and fire both. "Hit by ranged" is the archer in particular, "hit by melee" the one at the follower's face, "hit by magic" any caster (2026-09-04, for the Attack action in docs/ACTIONS.md 6 and for armour buffs against blows).
2. A sink on `TESMagicEffectApplyEvent` for effects that skip the hit event
   (`kNoHitEvent`): cloaks, hazards, spit. Which ones do is to be tested.
3. The active-effect scan of section 2, with the effect's `caster` as the
   attacker, for anything still running.

The window is 3 s. "Hit by" (named Attacked by until 2026-09-09) is true within it; the attacker is the
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
  resistances, hit-by bits and the attacker -- on the follower, the
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
- The `armor ...:` line the sensors log once per actor, at vanilla settings: that `GetArmorBaseFactorSum` reads as pieces x 0.03 (0.06 for robes and boots), so the reduction in the sheet's Armor row matches the 6% / 10% seen with the old slot count. Read in Nordic Souls on 2026-09-11, where fArmorBaseFactor is 0: the sum was 0.000 over 3 pieces, `CalcArmorRating` 292.5 was the pieces alone, and the `kDamageResist` value 392.5 the pieces plus a +100 Fortify Armor Rating enchantment, which is why the value is the rating now. The line keeps `CalcArmorRating` beside the value as the cross-check. And the logged values for a fight's enemies, to replace the estimated tiers.

## Sources

UESP Armor, Resist Magic, Followers; CK wiki Actor Value, OnHit, List of
Animation Variables; CS wiki fArmorRatingMax; Armor Rating Rescaled and
True Armour Rating (Nexus); alandtse/CommonLibSSE-NG `Actor.cpp`,
`MagicTarget.cpp` and the headers in this build tree;
Skyrim.esm records read through houseCARL (the 112 damage-keyworded
effects, the `AbResist*` abilities, the armour settings and base ratings).

## 7. Weapon (built and played 2026-09-08)

The follower's own weapons, hand by hand, under one "Weapon" heading with two entries.

**Charge needed** holds when an enchanted weapon in hand cannot pay for its next hit: its charge is below its enchantment's cost. One hit draws one fixed number, the enchantment's cost, whatever the attack; the game's own "Uses" figure is the charge divided by it. So the test is definite, and there is no percentage to choose. A weapon never used carries no ExtraCharge and reads as full. The pair is `Self: Weapon charge needed -> Self: Charge with strongest soul gem`.

**Poison: None / Active** holds when a weapon in hand takes a poison (anything but a staff) and has none on it, or carries one. With a poisoned sword right and a clean dagger left both hold, and with a spell right and a dagger left the dagger is what is asked about. The pair is `Self: Weapon poison none -> Self: Apply weakest health poison`; the action goes to the right hand's weapon if that is clean, else the left's, so two firings dress both hands, and with both poisoned it reports "poisoned" and waits. The engine's own inventory menu is stricter: it poisons the right hand only and never the left (`docs/ACTIONS.md`).

Self only: the snapshot reads the follower's own hands. The wire names are `weapon-charge-needed`, `weapon-poison-none` and `weapon-poison-active` (the last two were briefly `weapon-unpoisoned` and `weapon-poisoned` on 2026-09-08; a save carrying those drops the rule with a warning).

## 9. The cascade as it reads (2026-09-08)

Under every subject the conditions come in groups with a divider between: Any; Health, Stamina, Magicka; Combat; (for Enemy) Attacking, Attacked by; Hit type, Hit by; Status; Weapon, Armor, Resistance; Summon. Corpse, a subject of its own, has None and Level -> Highest, Lowest. Any is offered for everyone, the player and an ally included: always true of them, and there so a rule can aim at them under the heading a reader looks for it. There is no Count of a group any more: an ally's changes too rarely to be a condition and the enemy's was not wanted (it went on 2026-09-08; a save carrying `count-at-least` drops the rule with a warning).

**Hit type** (Using until 2026-09-09) is what the subject hits with, asked with the same kinds as Hit by but one: Melee (a blade, an axe, a mace), Ranged (a bow or crossbow), Magic (a spell or a staff), then Fire, Frost, Shock, Poison for whatever in hand does that kind of damage -- a weapon's enchantment, a staff's or a spell's effects, a poison on the blade -- read by what resists the effect, as a hit is. Hands with no weapon and no spell in them are Melee: the fists, and a creature's claws, teeth and horns, whose hands hold nothing (a bear read as nothing until 2026-09-09). Any subject, from the snapshot's traits; the wire name is `hit-type`, the kind under `"damage"` as for Hit by (which the writer left out until 2026-09-09, so every saved Hit type rule came back as the field's default, Fire).

**There is no Hit type: Any (removed 2026-09-09).** Because hands holding nothing read as Melee, every actor hits with something, so the condition was true of everyone: the plain Any condition wearing a heading that promises a filter. `IsDamageKindValidFor` refuses it, so a profile carrying one reports InvalidCondition instead of firing on every tick. Hit by: Any is a different question and stays -- hit with anything at all *inside the 3 s window*, false of an enemy nobody has touched, and it binds the Attacker for the Then side. Ally: Any stays too: it is always true, but Any is the panel's "no condition" entry and it is also what names the target, binding the nearest ally for the action to aim at.

The player is "Player" everywhere in the panel, never by name: a long name breaks the layout.

**Statuses** that no action could answer are not asked about the follower themself: bleeding out, casting, fleeing and staggered. The player neither bleeds out nor flees. About anyone else every status is a fair question (`IsStatusValidFor`).

**Attacking and Attacked by** (Targeting and Target of until 2026-09-09) replace the old "attacking player" and "target of player": each opens on the members of the party -- Self, Player, the other followers by name -- so `Enemy: Attacked by Player` is the one the player is fighting and `Enemy: Attacking <Lydia>` the one going for Lydia. The member goes on the wire as `"member": "player"` or the follower's form; the wire names are `attacking` and `attacked-by`. `Enemy: Attacked by <this follower>` is the follower's own target, which was a subject of its own ("Target") until 2026-09-08; one place for one question.

**On the action side** there is one Enemy heading, read from the condition: under an enemy condition it is the enemy the condition matched; under any other, whoever the follower is fighting, and failing that the nearest enemy sensed, since a cast must go at someone and nearest is what the follower's own AI picks. The separate "Target" heading is gone with the subject. Attacker stays: whoever last hit the actor the condition bound. It is not offered under an enemy or a corpse condition, and `IsActionTargetValidFor` refuses it there: an enemy's attacker is one of the party, and no rule means to aim at that. The other way round is a condition, `Enemy: Attacking <member>`, which binds the enemy on a party member. Under an enemy condition only Enemy (and self, the player, the followers) are offered.

## 10. Who a record's condition is asked of (read 2026-09-13)

Not the rule conditions above: the Creation Kit conditions on records -- a spell's effect, a magic effect, a perk's entry -- that the panel's pages list under an effect. Each runs on a party (Subject, Target, a named Reference, Combat Target, Linked Reference, Quest Alias, Package Data, Event Data, Command Target) and may carry a flag swapping Subject and Target; who Subject and Target *are* depends on what asks. Until 2026-09-13 the panel asked every list with the follower as both, and greyed Adamant's Bastion Dragonhide on a follower as "Conditions not met" while the Character sheet counted its +200.

### What the engine does

Read from the running 1.6.1170 executable (Nordic Souls, `tools/livedisasm.py`); the IDs are the Address Library's.

- **A running effect is re-checked** by ID 34062, from `ActiveEffect::EvaluateConditions` (34063, vtable slot 05): `effect->conditions.IsTrue(target->GetTargetStatsObject(), caster)`. **The Subject is the actor the effect is on, the Target whoever cast it.** The answer goes to `conditionStatus`, and 34063 sets `kInactive` while it is false. Paced by elapsed time against a setting not yet named.
- **Only the spell entry's list is re-checked**, and only with `kHasConditions` set, which the constructor (34049) sets exactly when that list is not empty. A magic effect record's own conditions (`EffectSetting::conditions`) are never in this check: asked when the effect lands, never again. Where they are asked on landing is not read; the only magic-code caller of `TESCondition::IsTrue` (29888) is 34062.
- **A perk entry** (`BGSEntryPointPerkEntry::CheckConditionFilters`, the vanilla body 23800) asks tab *i* as `IsTrue(argument i as a reference, nullptr)`: a base-form argument, a weapon or a spell, is wrapped in one shared dummy reference so reference functions work on it; a tab whose argument is missing is not asked and holds only if it has no conditions; and the Target is null on every tab. So tab 0's Subject is the owner, and anything on its Target is false.
- **One condition** (`TESConditionItem::IsTrue`, 29924) runs on: the Subject for Subject; the Target for Target; the named reference for Reference; the Subject's combat target for Combat Target; the Subject's linked reference for Linked Reference; the context's quest alias, package data or story event for those three; and Command Target falls to the switch's default, the Subject (unexpected, unverified). The swap flag (bit 4) trades the two parties when both are there. **A reference function with nothing to run on is false, its handler never called** -- which is not the same as "not met".
- **`EffectWasDualCast`** (handler 21719) reads `Actor::boolFlags` `kCheckAddEffectDualCast` (bit 30) on the actor it runs on, 0 for anything else: a flag held only while a dual-cast effect is being added. Asked at any other moment it is 0.
- **Two detours in Nordic Souls** stand in front of these: `BugFixesSSE.dll` detours 34062 and `PerkEntryPointExtender.dll` replaces the perk entry's slot 00. The bodies above are vanilla; what those two change is not read.

### The Bastion case

Adamant.esp gives Dragonhide (0CDB70) a fourth effect, `MAG_PerkBastionArmorFFSelfArea` "Armor - Bastion" (09862F): Self, area 50, +200 DamageResist, no conditions on the entry. The magic effect record carries three: `EffectWasDualCast = 1` on Target, `HasPerk(MAG_Bastion) = 1` on Target, `GetShouldAttack(Player) = 0` on Subject. The perk reads "Protection spells like Oakflesh and Fire Shell affect nearby allies when dual cast." Asked as the engine asks on landing -- Subject the ally in the area, Target the player who cast it -- all three hold. Asked with the player as Target afterwards, `HasPerk` and `GetShouldAttack` still hold and `EffectWasDualCast` does not, and never will: the list is the record's and is not asked again. **So asking conditions again cannot say whether an effect is acting; only the engine's flag can.**

### Who the parties are, by page

| Page | Subject | Target |
|---|---|---|
| Effects tab, an effect running on the follower | the follower | whoever cast it (`ActiveEffect::caster`); nobody once the caster is gone |
| a Self spell, shout or scroll; a potion, food, an ingredient; a worn enchantment | the follower | the follower |
| an aimed spell or scroll, a weapon's enchantment, a poison on a blade | whoever it hits: for a hostile effect the enemy the follower is fighting, else nobody the page can name | the follower |
| a perk, tab 0 | the owner | nobody (false in the engine) |
| a perk, other tabs | the entry's argument | nobody |

`AlchemyItem::GetDelivery` answers Self for every potion and every poison alike, so a poison is told by `IsPoison()`.

### What the panel does (built 2026-09-13, not yet seen in play)

- **An effect on the Effects tab is Inactive by the engine's flag**, `kInactive` or `kDispelled`, which the sheet's totals read too, so the list and the totals cannot disagree. Its page greys the row "Inactive" by the same flag; the conditions beneath are for reference.
- **The conditions are asked of the parties in the table** (`ConditionParties` in `Sensors.h`), and each names the one it runs on, after the swap flag: "HasPerk(Bastion) on Player", "GetShouldAttack(Player) on Lydia". A party nobody can name -- the Subject of an aimed spell out of a fight, the caster once gone -- keeps the Creation Kit's word, "on Subject" or "on Target".
- **Under Met a tick is true and blank is false.** **N/A** is a condition that needs a party who is not there -- no enemy being fought, the caster gone -- whose engine answer would be the false of asking nobody. **?** is one whose answer cannot be known from a sheet: `EffectWasDualCast`, which reads a flag held only while an effect is added and is 0 afterwards. An effect with either under it gets no "Conditions not met" verdict.
- **In a fight, a hostile aimed effect, a weapon's enchantment or a poison is asked of the enemy the follower is fighting** (their live combat target); out of one, of nobody.
- **Perk pages are unchanged**, still asking the owner as both parties: the engine asks tab 0 with no Target, but in Nordic Souls `PerkEntryPointExtender` replaces that check, and what it does is unread.

### To verify

- In play: Bastion Dragonhide on a follower reads active, `HasPerk` and `GetShouldAttack` ticked and `EffectWasDualCast` a ?; a weapon's enchantment page ticks against the enemy in a fight and shows N/A out of one.
- Where the engine asks a magic effect record's conditions on landing, and with what; expected the same two parties with `kCheckAddEffectDualCast` set on the caster.
- What `BugFixesSSE` and `PerkEntryPointExtender` change; the setting pacing the re-check; Command Target falling to the Subject; whether any perk in the load order puts a Target condition on tab 0.
- The rows do not show the swap flag.
