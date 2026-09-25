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
| Status | poisoned, burning, frostbitten, shocked, paralyzed, staggered, fleeing, bleeding out, invisible, ethereal, blocking, casting, sneaking, diseased, bleeding (not all about everyone, see 9) | none |
| Resistance | fire, frost, shock, magic, poison | a percent, below or above (0 included, see 4); or lowest / highest |
| Hit by | any; melee, ranged, magic; fire, frost, shock, poison | none |
| Attacks with (`hit-type`) | melee, ranged, magic; fire, frost, shock, poison -- no "any", see 9 | none |
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
| burning | an effect that damages the actor it runs on and is resisted by fire: detrimental, a value-lowering archetype (`kValueModifier`, `kPeakValueModifier`, `kDualValueModifier`, `kAbsorb`), `resistVariable` `kResistFire`; see below | high |
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
| diseased | an effect whose `spell->GetSpellType()` is `kDisease` (added 2026-09-18) | medium: vanilla diseases are Disease-type spells and show as active effects; not yet seen in play |
| bleeding | an effect whose base effect is a bleed: Skyrim.esm's `PerkBleedingDamage` 0x0C367A, any effect of its name, the Targe of the Blooded's 0x10582C, or any carrying the keyword `MAG_MagicDamageBleed` (added 2026-09-25, issue #1) | medium: seen in play 2026-09-25 for the Targe's (the player's bash, read by a follower's `Enemy: Bleeding`); the axes', by name and by keyword not yet; see below |

**Burning, frostbitten, shocked** are the element's damage to the actor. The engine has no "on fire" state: a fire damage effect tapers, going on after the hit at a falling magnitude for 0.1 to 4 seconds by spell (UESP, Skyrim:Fire_Damage), and that tail is the burn. So the signal is the record: resisted by the element, which is what the engine reduces it by, and lowering a value of the actor it runs on. The keyword `MagicDamageFire` is not used: in vanilla it also tags Light Damage, Stamina Damage, sun damage and Mora's Curse, and mods leave it off. Until 2026-09-25 any detrimental effect resisted by the element counted, and in the base game and DLC that held 22 effects that damage no one they run on (read with houseCARL): an atronach's cloak on the atronach, which harms whoever comes near through an effect of its own, and so read every flame atronach as always Burning, frost atronachs and death hounds Frostbitten, storm atronachs Shocked; J'zargo's cloak on J'zargo; the bound weapons' visuals; script effects (Summon Ancients, a ritual, a Stop Rune). A script that deals the element's damage itself would now be missed; none in the base game does. Vanilla's own mislabels stay: electrified water is resisted by fire.

**Bleeding** is not a state the engine knows. Vanilla's is one effect, Bleeding Damage (`PerkBleedingDamage`): the Hack and Slash and Limbsplitter perks apply it to a war axe's or battleaxe's hit, one spell per material and rank (`PerkBleedingSword*`), 1 to 3 points a second for 3 to 8 seconds, hidden in the UI, not resisted, and the target visibly bleeds. It is an unresisted Damage Health, and so are nearly a hundred effects in the base game and DLC that are not bleeds (read with houseCARL, 2026-09-25), so no field tells one apart and the record is the signal. The Targe of the Blooded's bash is the one exception, counted by its record alone (`dunTargeOfTheBloodedME`, 0x10582C): named Damage Health, as hundreds of effects are, and described as bleeding damage, so the player reads it as one; Vigilant's parrying dagger uses it too. UESP calls it not true bleed damage (it stacks, and works on the undead), which a rule has no use for. A bleed that fits none of these ways is not one. **Who can bleed an enemy** is another matter: the Targe carries no enchantment and no script, and its bleed is a perk on the wearer (`dunTargeOfTheBloodedPerk` 0x10582A, `ApplyBashingSpell` with the Targe's spell) on two records only, the player's and Umana's, so a follower bashing with it bleeds nothing (2026-09-25: Jenassa's bash landed and the bear never bled; the player's bash was read at once). The axes are the same: Hack and Slash and Limbsplitter bleed only for an actor whose record or progression has the perk. Some mods reuse vanilla's effect in spells of their own (Vigilant, Unslaad); others add their own records, which are found two ways, each catching what the other misses in Nordic Souls. **By name:** the Redguard CC, Thaumaturgy, Artificer, the Masterwork weapons and Natura's spriggans name theirs Bleeding Damage, as vanilla's is named; the name is vanilla's record's in the game's language, so a mod left in English in a translated game is not matched by it. **By keyword:** Simonrim's mods share `MAG_MagicDamageBleed` (injected into Update.esm at 0xADA119), which is the only mark on Adamant's axe wound (`MAG_AxeWoundFFContact`) and DragonWar's bite, both named Damage Health, and which Blade and Blunt's `MAG_BleedControllerPerk` reads (`EPMagic_SpellHasKeyword`) to cut an incoming bleed by armour. The set is built on first use, and the log at debug says `bleeding: N effect(s)` and how many each way. A hidden bleed counts: vanilla's is hidden.

The vanilla condition functions (`HasMagicEffectKeyword`, `IsStaggered`,
`GetAttacked`...) can be built at runtime as a `TESConditionItem`, but the
direct reads above are what they read, so they are kept as a debugging
cross-check, not the hot path.

Snapshot: a `status` bit set per actor view. With the log at debug, each change of an actor's statuses is a line of its own, `Cave Bear (FF000C1A): now bleeding; no longer staggered`, whoever's snapshot read it first (`LogStatusChanges` in `src/game/Traits.cpp`, since 2026-09-25): without it a Status rule that never holds cannot tell a status not detected from one that never happened.

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

## 2b. Effect: what runs on the subject (built and reworked 2026-09-23, not yet seen in play)

`Effect`, right below Status: an effect of that name runs on the subject (`PredicateKind::EffectRunning`, one base effect's form in `Rule::conditionForm`, `"effect"` in the profile), whatever applied it. Reads as a status does, `Self: Oakflesh`. Any subject; negatable.

- **Offered as one list of the effects the page's actor can put up**, by name: what the potions and food carried, the spells known and scrolls carried, the shouts and the powers leave running. How an effect is applied is not the question, so there are no headings. A condition on an effect nobody here can put up, or on one that is always on (Meditation's perk), has nothing to act on. Each thing counts by one effect, its main lasting one (below). The list is core's (`ArrangeEffectPicks`, `core/Effects.h`, tested): one pick per name, the first record given, by name.
- **Matched by name, not by record.** Nineteen records in Nordic Souls are called Fortify Health Regeneration: a potion's, a Gourmet food's, a ring's enchantment, altar blessings, survival mode's. The rule keeps one record, and the traits carry, for each running effect, every record of its name (`ActorTraits::effects`, from an index of the load order's effects by name built on first use), so any of them answers. A ring of it counts, which means "drink it if it is not running" does not top up while the ring is on, though the potion would stack. A hidden effect counts for nothing: survival mode's hidden bookkeeping of the same name would otherwise hold the condition true unseen. The name is taken from the record at load, so a save played in another language still reads its rules.
- **What is listed:** a thing whose effect lasts more than a second, which a tick half a second apart can see (`LastingEffect` in `src/game/Effects.cpp`: the shown effect before a hidden one, then the costliest). Firebolt, Fast Healing and Restore Health are not. A script effect of a second, which a power or a teleport uses only to run its script (Whistle to Inigo), does not. A hidden effect is not listed. A spell, a scroll or a shout only when cast on oneself: an aimed one leaves its effect on the target, and a drain's share on the caster is not what it is cast for; what an aimed spell does to an enemy is mostly a Status. No summon anywhere, a power's included: that is the Summon condition's. A shout by its highest unlocked word's effect, the word a Shout action shouts.
- **Toggles.** Blood Sacrifice's power leaves one script effect of no duration; its script (`dar_simpletogglescript`) adds or removes the ability `BLO_abBloodSacrifice`, which is what runs. The link is the script's property (`TriggerSpell0`), which the engine hands to the script engine at load and keeps nowhere CommonLib reads, so `src/game/Toggles.cpp` reads it back from the plugin at data load: for every spell or power with nothing lasting of its own and a script effect, the effect's record in the plugin that last defines it, its VMAD, and the first property naming an ability. The byte reading is core's (`core/PluginFile.h`, tested); the file's own form numbers are put in the load order's through its masters. The power is then listed under Power by the ability's effect, and `IF Self: Blood Sacrifice THEN Use power: Blood Sacrifice`, in the idle list, turns it off. The log says `toggle: X turns on Y` for each, and one summary line. A compressed effect record is skipped and counted, not read (it needs zlib).
- **History.** An Exclusive condition on "dispel with keywords" families sat beside this from 2026-09-23 and was taken out the same day: nothing asked for it, and the families the load order yields are mostly a spell's own keyword or a stray tag. A watcher on effects landing was the first try at toggles; it never logged a link in play.

## 2c. Location: where the follower is (built 2026-09-25 on `wip-location`, not yet seen in play)

`Location`, a heading of its own at the end of the cascade (`PredicateKind::Location`, the kind in `Rule::locationKind`, `"location": "draugr-crypt"` in the profile). Self only: the party stands in one place, and on the player's page Self is the player. The idle list's alone, as asked: the combat list does not offer it, and reports one a hand-edited profile carries as an invalid condition. Negatable. It reads "Self: In Draugr crypt", "Self: In Whiterun", a hold by its name alone. Asked on Nexus, issue #6, where the analysis below is also posted.

```
Location
  Home
  Interior
  Exterior
  ---
  Building   > Any, Castle, Guild, House, Inn, Store, Temple
  Cave
  Dungeon    > Any, Animal den, Bandit camp, Dragon lair, Dragon priest lair, Draugr crypt, Falmer hive,
               Forsworn camp, Giant camp, Hagraven nest, Riekling camp, Spriggan grove, Vampire lair,
               Warlock lair, Werebear lair, Werewolf lair
  Fort
  Hold       > the load order's holds, by the game's names
  Ruin       > Any, Dwarven ruin, Nordic ruin
  Settlement > Any, City, Town, Orc stronghold
```

The three first in that order; the headings after the divider, and each heading's kinds, by name in the language shown. The grouping is core's (`LocationGroup`, `GroupOf`, `IsGroupAny` in `core/Kinds.h`, tested).

- **Interior / Exterior** are the cell's (`TESObjectCELL::IsInteriorCell`). Two kinds rather than one and its Not: an actor with no cell under them is neither. A cell that shows the sky is still an interior, as the engine has it.
- **Every other kind but Hold is a keyword** on the actor's current location or one it lies in. A location does not carry the keywords of its parent, so the chain is walked: in Breezehome the follower is in a House and a Home, in a City and a Settlement (Whiterun), and in Whiterun Hold. Which keywords mark which kind is `kSources` in `src/game/Places.cpp`, with a compile-time check that every kind is read.
- **Home** is `LocTypePlayerHouse` (Skyrim.esm 0FC1A3): 35 locations in Nordic Souls, the five town houses, Severin Manor, the Arch-Mage's Quarters, the Hearthfire and Creation Club homes, mods' own. The Hearthfire houses carry it on their interiors only: the grounds are `BYOH_LocTypeHomestead`, so the yard of Lakeview Manor is not Home.
- **Cave** is `LocSetCave` or `LocSetCaveIce` (some of Dragonborn's ice caves carry only the second), a group of one. Ice is the only kind of cave the keywords name, and it was left out (2026-09-25): what sets one cave apart from another is who holds it, which every cave carries as a Dungeon kind -- Embershard `LocTypeBanditCamp`, Broken Fang Cave `LocTypeVampireLair`, Chillwind Depths `LocTypeFalmerHive`, Crystaldrift Cave `LocTypeAnimalDen`, Moss Mother Cavern `LocTypeSprigganGrove`, Harmugstahl `LocTypeWarlockLair`.
- **Dungeon**'s Any is `LocTypeDungeon` (300 locations). Its kinds are who holds the place, `LocType*` of that name, indoors or out, and apart from what it is built as: a giant camp in the open counts under it, and "a cave held by bandits" cannot be asked, only "Bandit camp". Riekling camp is Dragonborn's `DLC2LocTypeRieklingCamp`, Werebear lair Dragonborn's `LocTypeWerebearLair`.
- **Fort** is `LocSetMilitaryFort` (keeps and towers: Northwatch Keep, Treva's Watch) or `LocTypeMilitaryFort` (the garrisoned forts: Fort Greymoor, Fort Amol), a group of one, since the keywords name no kinds of fort. Vanilla also puts the second on Broken Fang Cave, Nightcaller Temple and Bloodlet Throne, which therefore read as forts.
- **Ruin** is `LocSetNordicRuin` (55), `LocSetDwarvenRuin` (24) or `LocTypeDwarvenAutomatons`; Dwarven ruin the last two, Nordic ruin the first.
- **Settlement**'s Any is `LocTypeHabitation`, `City`, `Town`, `Settlement` or `OrcStronghold` -- the last three include the farms, mills and hamlets. City: Whiterun, Solitude, Markarth, Riften, Windhelm, Raven Rock, and Beyond Skyrim's Bruma. Town: Riverwood, Rorikstead, Dawnstar, Falkreath, Morthal, Winterhold, Ivarstead, Dragon Bridge, Karthwasten, Shor's Stone, Skaal Village (and, as vanilla has it, the two Dark Brotherhood sanctuaries).
- **Building**: `LocTypeCastle`, `Guild`, `House` (anyone's, the player's included), `Inn` (27), `Store`, `Temple`. Its Any is any of those, or `LocTypeDwelling`, `LocTypeBarracks` or `LocTypeJail`, which are not listed on their own; no one keyword covers every building.
- **Hold** is a location record, not a keyword: the nearest location up the chain marked `LocTypeHold` is the follower's (`Snapshot::hold`), and the rule names one (`Rule::conditionForm`, `"hold"` in the profile, as a form, so another load order's Whiterun reads back as its own). Listed from the load order at first use (`Holds`), by the game's own name, in the game's language: the nine, Solstheim, and a mod's own (Beyond Skyrim's County Bruma and Heartlands, Wyrmstooth, Hjorkvild Isles).

Read on the tick into `Snapshot::places`, a bit per kind, and `Snapshot::hold` (`ReadPlaces`, `src/game/Places.cpp`). With the log at debug each change is a line: `Jenassa (000E1BA9): now at home, interior, house, settlement, city; hold Whiterun`.

**Left out**, each one more kind and one keyword if wanted: Farm, Mine, Lumber mill, Jail, Barracks, Cemetery, Ship, Shipwreck, Stables (USSEP), Military camp, Outdoor, the hold ranks (`LocTypeHoldCapital`, `Major`, `Minor`), Ash spawn (Dragonborn), and Cleared -- the engine keeps whether a clearable location has been cleared (`BGSLocation::IsCleared`), which is state rather than a keyword. A mod's dungeon without the vanilla keywords is not recognised; outside Skyrim.esm and the DLC the families are thin (Beyond Skyrim's `CYRLocType*`, Saints and Seducers', one Creation Club Ayleid ruin).

**To see in play:** the places logged at debug against where the follower stands; whether the ground outside a dungeon's door already counts as the dungeon (its exterior cells may belong to its location); the hold list's names in a translated game.

## 3. Armor

Damage reduction = min(fMaxArmorRating 80%, rating x fArmorScalingFactor 0.12 / 100 + pieces x fArmorBaseFactor 0.03). The cap is reached at a displayed 567 with four pieces. The rating is the `kDamageResist` actor value, the displayed figure: the worn armour with tempering, skill and perks, plus every effect running on the value, a Fortify Armor Rating enchantment, a potion, a flesh spell. Not `Actor::CalcArmorRating()`, which is the pieces alone and was the input from 2026-09-04 to 2026-09-11: Frea in Nordic Carved with a +100 Fortify Armor Rating helmet read 392.5 on the value and 292.5 there, and the sheet listed the enchantment in its tooltip against a total without it. The value is written when the pieces change and can trail a skill gained since, which is accepted. The hidden bonus is `Actor::GetArmorBaseFactorSum()`. Only the combination is ours, because the engine does it inline in the hit handler (44014), where it was read on 1.6.1170 (2026-09-24): the same sum and cap, with two differences by decision. The attacker's Mod Target Damage Resistance perks (entry point 0x25), which the handler applies before the cap, are left out: the condition is about the actor's armour, not one attacker's blow. And the handler sets no floor, so a negative rating makes a blow do more; ours stops at 0, since a negative rating is an oversight in the engine's arithmetic, not a state to write rules for. A mod that changes the settings or the ratings is reflected; one that hooks the formula itself (Armor Rating Rescaled, Armor Rating Redux) is not. The snapshot carries the reduction as a fraction, not the displayed number, so the condition means the same thing on a bandit in fur and a chief in plate.

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
   `IsPoison()` as well, so a fire hit is magic and fire both. "Hit by ranged" is the archer in particular, "hit by melee" the one at the follower's face, "hit by magic" any caster (2026-09-04, for the Attack action in dev/ACTIONS.md 6 and for armour buffs against blows).
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

**The Summons tab**, after Shouts, shows what the follower commands: a chip per summon carrying its name, then health, stamina and magicka bars, level, whether summoned or raised, seconds remaining on the commanding effect, the reference and base FormIDs, and the same sheet the Character tab builds for the follower.

## 7. What was built, where

- `core/Kinds.h`: StatusKind, ArmorBand and BandOf, DamageKind, ResistBand.
- `core/Snapshot.h`: ActorTraits -- status bits, armour reduction, six
  resistances, hit-by bits and the attacker -- on the follower, the
  player, each ally and each enemy.
- `core/Rule.h`: predicates Status, Armor, Resistance, AttackedBy,
  HealthLowest/Highest, ArmorLowest/Highest; the rule's statusKind and
  damageKind; SubjectKind::Follower with subjectForm; the Attacker target.
- `game/Traits.cpp`: ReadTraits; `game/Values.cpp`: DamageReduction;
  `game/Sensors.cpp`: the party and enemy walk. `game/Hits.cpp`: the hit table and its two sinks.
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

The follower's own weapons, hand by hand, under one "Weapon" heading with three entries, by name in the language shown, as the statuses are.

**Charge needed** holds when an enchanted weapon in hand cannot pay for its next hit: its charge is below its enchantment's cost. One hit draws one fixed number, the enchantment's cost, whatever the attack; the game's own "Uses" figure is the charge divided by it. So the test is definite, and there is no percentage to choose. A weapon never used carries no ExtraCharge and reads as full. The pair is `Self: Weapon charge needed -> Self: Charge with strongest soul gem`.

**Poison: None / Active** holds when a weapon in hand takes a poison (anything but a staff) and has none on it, or carries one. With a poisoned sword right and a clean dagger left both hold, and with a spell right and a dagger left the dagger is what is asked about. The pair is `Self: Weapon poison none -> Self: Apply weakest health poison`; the action goes to the right hand's weapon if that is clean, else the left's, so two firings dress both hands, and with both poisoned it reports "poisoned" and waits. The engine's own inventory menu is stricter: it poisons the right hand only and never the left (`dev/ACTIONS.md`).

**Bound: None / Active** holds when neither hand holds a bound weapon, or either does (added 2026-09-25, issue #3; seen in play the same day). What makes one bound is the Bound Weapon flag on the weapon record (`TESObjectWEAP::IsBound`, `flags2` bit 13), which every weapon a Bound-archetype effect conjures carries: all 61 such effects in Nordic Souls (vanilla, Dragonborn, and BSAssets, Vigilant, Unslaad, Mysticism, Adamant, Necrotic, Dragon Cult Draugr) name a weapon with it, the renamed ones too -- Necrotic's, the Flame Bow, the Bow of Meridia -- so neither a name nor a keyword is asked. The weapon in hand, not the effect: the engine takes the weapon away when the effect ends and ends the effect when the weapon is put away, so the two agree, and the hand is what the condition is about. The pair is `Self: Bound weapon none -> Self: Cast Bound Sword`; a bound bow sits in the right hand as any bow does.

Self only: the snapshot reads the follower's own hands. The wire names are `weapon-charge-needed`, `weapon-poison-none`, `weapon-poison-active`, `weapon-bound-none` and `weapon-bound-active` (the last two were briefly `weapon-unpoisoned` and `weapon-poisoned` on 2026-09-08; a save carrying those drops the rule with a warning).

**Arrows: None / Available**, a heading of its own after Weapon (added 2026-09-25, issue #4; seen in play the same day): the follower carries no ammunition, or some. Arrows and bolts both count, as the Equip arrows actions choose from both; whether they suit the bow or crossbow in hand is not asked. Read off the loadout the pin book already lists (`Snapshot::CarriesAmmo`). Self only. In vanilla a follower seldom runs out: they are given a hunting bow and twelve iron arrows when recruited, and one with a single better arrow uses it indefinitely (UESP, Skyrim:Followers). So it matters most for the player under tactics, who spends arrows, and under a mod that has followers spend theirs. The wire names are `arrows-none` and `arrows-available`.

## 9. The cascade as it reads (2026-09-08)

Under every subject the conditions come in groups with a divider between: Any; Combat; Health, Stamina, Magicka; (for Enemy) Attacking, Attacked by; Attacks with, Hit by; Status; Weapon, Armor, Resistance; Summon; (for Self, in the idle list) Location. Corpse, a subject of its own, has None and Level -> Highest, Lowest. Any is offered for everyone, the player and an ally included: always true of them, and there so a rule can aim at them under the heading a reader looks for it. There is no Count of a group any more: an ally's changes too rarely to be a condition and the enemy's was not wanted (it went on 2026-09-08; a save carrying `count-at-least` drops the rule with a warning).

**Attacks with** (Using until 2026-09-09, then Hit type until 2026-09-17; the wire name is and stays `hit-type`) is what the subject hits with, asked with the same kinds as Hit by but one: Melee (a blade, an axe, a mace), Ranged (a bow or crossbow), Magic (a spell or a staff), then Fire, Frost, Shock, Poison for whatever in hand does that kind of damage -- a weapon's enchantment, a staff's or a spell's effects, a poison on the blade -- read by what resists the effect, as a hit is. Hands with no weapon and no spell in them are Melee: the fists, and a creature's claws, teeth and horns, whose hands hold nothing (a bear read as nothing until 2026-09-09). Any subject, from the snapshot's traits; the wire name is `hit-type`, the kind under `"damage"` as for Hit by (which the writer left out until 2026-09-09, so every saved Hit type rule came back as the field's default, Fire).

**There is no Hit type: Any (removed 2026-09-09).** Because hands holding nothing read as Melee, every actor hits with something, so the condition was true of everyone: the plain Any condition wearing a heading that promises a filter. `IsDamageKindValidFor` refuses it, so a profile carrying one reports InvalidCondition instead of firing on every tick. Hit by: Any is a different question and stays -- hit with anything at all *inside the 3 s window*, false of an enemy nobody has touched, and it binds the Attacker for the Then side. Ally: Any stays too: it is always true, but Any is the panel's "no condition" entry and it is also what names the target, binding the nearest ally for the action to aim at.

**The explanation of each of these two sits on its heading, not on its kinds** (2026-09-17): *Fights with this type of damage* and *Hit by this type of damage in the last few seconds*, once each, since the same sentence over Fire and over Melee only gets in the way of reading the list. `BeginCascade` takes the heading's hover text and reads the hover itself, because with the submenu open the last item is the popup's rather than the heading's.

The player is "Player" everywhere in the panel, never by name: a long name breaks the layout.

**Statuses** that no action could answer are not asked about the follower themself: bleeding out, casting, fleeing and staggered. The player neither bleeds out nor flees. About anyone else every status is a fair question (`IsStatusValidFor`).

**The idle list** (2026-09-18, `dev/PLAYER.md` "Out of combat") offers less, since there is no enemy out of a fight: no Enemy, and no Attacking or Attacked by with it; no Combat start or end; no Hit by; no Enemy or Attacker target; no Attack or blow; no Fleeing. The combat list, the other way round, has no Location (2c). The `IsXValidIn(moment, ...)` five in `core/Rule.h` say so, the menus read them, and the evaluator reports a rule of one as an invalid condition.

**Attacking and Attacked by** (Targeting and Target of until 2026-09-09) replace the old "attacking player" and "target of player": each opens on the members of the party -- Self, Player, the other followers by name -- so `Enemy: Attacked by Player` is the one the player is fighting and `Enemy: Attacking <Lydia>` the one going for Lydia. The member goes on the wire as `"member": "player"` or the follower's form; the wire names are `attacking` and `attacked-by`. `Enemy: Attacked by <this follower>` is the follower's own target, which was a subject of its own ("Target") until 2026-09-08; one place for one question.

**On the action side** there is one Enemy heading, read from the condition: under an enemy condition it is the enemy the condition matched; under any other, whoever the follower is fighting, and failing that the nearest enemy sensed, since a cast must go at someone and nearest is what the follower's own AI picks. The separate "Target" heading is gone with the subject. Attacker stays: whoever last hit the actor the condition bound. It is not offered under an enemy or a corpse condition, and `IsActionTargetValidFor` refuses it there: an enemy's attacker is one of the party, and no rule means to aim at that. The other way round is a condition, `Enemy: Attacking <member>`, which binds the enemy on a party member. Under an enemy condition only Enemy (and self, the player, the followers) are offered.

## 9a. Not: the condition negated (built 2026-09-17, reread 2026-09-18, not yet seen in play)

A **Not** column sits between the rule's number and its condition, a tick like the On switch: ticked, the condition is asked the other way round of each actor the subject names. "An enemy that is not undead", "the player is not sneaking", "this weapon is not poisoned" are each one rule now, where before they needed a predicate of their own -- which is why the pairs that exist (Summon none / active, Weapon poison none / active) exist at all. The tooltip says *Click to negate condition* and, once ticked, *Click to unnegate condition*. On the wire it is `"not": true` inside the rule's `if`, written only when it is on.

**A negated condition binds one the plain condition does not hold of.** The Not is applied per actor, inside the match (`MemberSatisfies`, `EvaluateSelf`), not to the group's answer: `NOT Enemy: Undead` is *an enemy that is not undead*, and binds that enemy, exactly as the plain rule binds an undead one -- so "Enemy NOT resists fire, cast Firebolt at Enemy" aims at the unresisting one. Everything downstream reads the binding as it does under the plain rule: the action targets that mean *the one the condition matched* (Ally, Enemy, Corpse) resolve to it, the log names it, `IsActionTargetValidFor` and `Reconcile` know nothing of the Not. Among several that qualify the **nearest** binds, whatever the measure: "not below 50%" names no direction to rank by (`Better`). Of the follower, the player or a named follower, Not is simply the condition's opposite; a named follower who is away matches nothing either way round.

Until 2026-09-18 the Not was applied to the group's answer instead -- `NOT Enemy: Undead` held while *no* enemy was undead, bound nobody, and needed exceptions in the target check, the target resolution and `Reconcile` to send the action to the follower. Dropped because the column sits beside the condition and reads per actor, and because the marquee use, hit the enemy that does not resist, needs the binding. "No enemy is X" is written as rule order now: the positive rule first, a catch-all after.

**The conditions that cannot be negated** (`CanNegate`) have a dead cell with a tooltip saying so: **Any**, which is the always-true condition and negates to a rule that can never fire; **Combat begins** / **Combat ends**, which are moments -- "not the tick the fight began" is every other tick of it, which is not what anyone means by Not; the **extremes** (lowest health, highest armour, the corpse's highest and lowest level), which are *which of the group*, not something one of them is or is not, so that per actor they hold of everyone and negate to nobody; and **Corpse: None**, a question about the group whose opposite is the plain Level condition. A profile hand-edited to carry one keeps the rule and drops the flag.

**The two guards around a condition are outside the negation**: a subject/predicate pair that cannot be asked at all stays unanswerable rather than becoming true, and the farewell evaluation after a fight still runs only Combat end rules. Per-actor negation gets this for free -- with no actor to ask there is nothing to hold -- but the tests pin it (`tests/test_conditions.cpp`, `[not]`), since a Not applied to the answer would fire on the tick after a fight and on every pair the editor refuses, the two places its plain twin is never even evaluated.

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
- **The conditions are asked of the parties in the table** (`ConditionParties` in `EffectRows.h`), and each names the one it runs on, after the swap flag: "HasPerk(Bastion) on Player", "GetShouldAttack(Player) on Lydia". A party nobody can name -- the Subject of an aimed spell out of a fight, the caster once gone -- keeps the Creation Kit's word, "on Subject" or "on Target".
- **Under Met a tick is true and blank is false.** **N/A** is a condition that needs a party who is not there -- no enemy being fought, the caster gone -- whose engine answer would be the false of asking nobody. **?** is one whose answer cannot be known from a sheet: `EffectWasDualCast`, which reads a flag held only while an effect is added and is 0 afterwards. An effect with either under it gets no "Conditions not met" verdict.
- **In a fight, a hostile aimed effect, a weapon's enchantment or a poison is asked of the enemy the follower is fighting** (their live combat target); out of one, of nobody.
- **Perk pages are unchanged**, still asking the owner as both parties: the engine asks tab 0 with no Target, but in Nordic Souls `PerkEntryPointExtender` replaces that check, and what it does is unread.

### To verify

- In play: Bastion Dragonhide on a follower reads active, `HasPerk` and `GetShouldAttack` ticked and `EffectWasDualCast` a ?; a weapon's enchantment page ticks against the enemy in a fight and shows N/A out of one.
- Where the engine asks a magic effect record's conditions on landing, and with what; expected the same two parties with `kCheckAddEffectDualCast` set on the caster.
- What `BugFixesSSE` and `PerkEntryPointExtender` change; the setting pacing the re-check; Command Target falling to the Subject; whether any perk in the load order puts a Target condition on tab 0.
- The rows do not show the swap flag.
