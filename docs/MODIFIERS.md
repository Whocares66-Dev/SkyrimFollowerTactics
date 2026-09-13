# Modifiers: where a follower's bonuses come from, and how to total them

Research and thoughts on one question: when the sheet says a follower does 50% more one-handed damage, or pays 20% less for spells, where does the number come from, and can every source be named? Started 2026-09-13 from a Nordic Souls session with a bard follower. `docs/RESEARCH.md` section 6 has the earlier findings on the hidden skill-boost perks that this builds on.

## An actor value does not know its sources

The engine keeps three floats per actor value: the permanent, temporary and damage modifiers. Whatever writes one of them, an effect, a race bonus, a script's ModAV, adds a number and walks away. There is no list of contributors. The provenance the Character sheet's hover note shows today is reconstructed by walking the actor's active effects and asking which modify the value; the "Perks:" line is the remainder nothing explained. That is the most the value can give, and it is already given.

## The value is a dial; the entry point is the quantity

Most of what a value like One-handed Modifier appears to do, it does not do. The damage formula never reads it. What reads it is a hidden perk every player carries, Perk Skill Boosts, whose Mod Attack Damage entry says "multiply by one plus a hundredth of the value". The value is a dial on a perk entry, and the perk entry is what the swing uses. The same holds for spell cost: no actor value carries "cost of all magic", and even the per-school Modifier values are dials on the same hidden perk's Mod Spell Cost entries. A mod that wants a cost discount registers its own entry at that hook, which is exactly what Adamant's Bard does (below).

So the quantity worth totalling is the entry point, not the value. Every source ends there:

- A plain perk (Armsman, and its Adamant equivalents) is a multiply entry.
- A Fortify enchantment or potion is a value, read by the hidden perk's "one plus a share of the value" entry, on an actor that holds that perk.
- A song, a stance, a condition of any kind is an entry gated by its conditions.

And the "is it available, is it hooked up" question answers itself at that level. A held perk whose conditions fail contributes nothing. A Fortify value on an actor without the hidden perk contributes nothing, because no entry reads it. A Fortify value on an actor with it appears as one factor, and that factor expands into the gauntlets and the potion through the value's own provenance.

## How the engine computes an entry point

When the engine prices a swing or a cast it starts from the base quantity, applies the skill curve, then hands the number to the entry point (`BGSEntryPoint::HandleEntryPoint`). That walks the perk owner's entries registered on that entry point (`Actor::ForEachPerkEntry`, a virtual, so the player and an NPC walk different storage), evaluates each entry's condition tabs against the call's arguments (tab one the perk owner, tab two the weapon or spell, tab three the target where there is one), and applies each surviving entry's function to the running value: set, add, multiply, add a range, or one of the actor-value forms. There is no actor value in the pipeline.

**The order, read from the running game (2026-09-13).** The Creation Kit wiki says an entry's Priority "controls the order of operations" and "a higher priority takes precedence", which does not say which runs first. The executable does, read with `tools/livedisasm.py` from the live Nordic Souls process:

- **The handler does no ordering.** `HandleEntryPoint` (id 23526) copies its arguments, asks the actor whether it has any entries for the point (vtable slot 0xFF, id 37701) and hands a visitor to the actor's walk (slot 0x100, id 37702). The player's vtable names the same walk on disk; in the running Nordic Souls process another plugin has hooked that slot for the player, and the perk entry's apply, remove and condition-check slots too. Which plugin, not known.
- **The entries live on the actor's process, one sorted array per entry point.** The walk goes through the actor's middle-high process's perk data (`MiddleHighProcessData::perkData`, 0x288: an `AIPerkData`, one `BSTArray<BGSPerkEntry*>` per entry point, 0x5C of them) and visits the array front to back (id 40025), stopping when a visitor says stop. An actor without a process, unloaded, has no entries and the handler applies nothing.
- **The array is kept sorted on insert.** The entry's apply (id 23806) calls the actor's add (37699, then 40022 on the process), which binary-searches the array with a comparison (id 23821) and inserts at the position found. The comparison reads the entry's priority byte (offset 9) and then its rank byte (offset 8): **higher priority sorts earlier; among equal priorities lower rank sorts earlier; a full tie lands wherever the binary search stops.** So the walk, and the application, run **highest priority first**. Only entry-point entries with an entry point below 0x5C are inserted. The perk data itself is made on the first add (0x8A0 bytes), so an actor gets it on the first perk applied.
- **How each function applies** (the visitor's Visit at rva 0x3860b0 checks the entry's conditions with the perk owner as tab one and the call's arguments after it, then dispatches on the function through a table of sixteen handlers, id 23528). Read for the ones a damage or cost figure meets, with `value` the running number:
  - Set, Add, Multiply: one-value data, the float at data+8. `value = v`, `value += v`, `value *= v`.
  - Add Range: two-value data, low at +8, high at +0xC. `value += low + (high - low) * random`, a fresh roll each call. Not reproducible; show as the range.
  - Add Actor Value Mult: two-value data, the actor value's index at +8 (stored as a float), the multiplier at +0xC. `value += owner.GetActorValue(av) * mult`, the owner's CURRENT value, temporary modifiers in, read through the owner's value interface; the owner must be an Actor (form type 0x3E) or the entry is skipped.
  - Multiply One Plus Actor Value Mult, the hidden skill-boost perks' form: same data. `value *= 1 + owner.GetActorValue(av) * mult`.
  - Set To / Multiply Actor Value Mult: same data, `value = av * mult` and `value *= av * mult`; not read line by line, inferred from their neighbours.

So the reproduction the strategy below asks for is well defined: the actor's entries for the point in array order, each entry's conditions against the same arguments, each surviving function applied as above. The one irreducible term is Add Range.

## Strategy: total from the engine, provenance from our own walk, and a check between them

1. **The total** comes from the engine's entry point call, as the weapon damage figure already does (`WeaponDamage` in `src/game/Sensors.cpp`). It is the number a swing would use, with every perk the engine would count and none it would not.
2. **The provenance** comes from our own walk of the same entries: `ForEachPerkEntry` on the actor for the entry point, evaluating each entry's conditions with the same arguments the total used, and formatting each surviving entry as one factor with its perk's name. The perk page's entry formatter (`EntryRow`) already words the functions; this needs the numbers.
3. **Only what applies is in the total.** An entry whose conditions fail against the arguments in hand is listed as conditional, with the condition named, and left out of the figure. That includes entries that read a target: outside a fight there is none, and the figure is the one the player's own inventory shows, which has no target either. When a target is known (a rule's evaluation, a fight) the same walk can be run with it and the figure changes, which the sheet should say.
4. **The self-check.** Our factors multiplied together in the order we walked them must equal the engine's figure. When they do not, log both with the entries, as the value note logs a value its listed sources do not explain (`LogUnappliedSources`). This is how a wrong ordering guess, or a function we format wrongly, shows itself.
5. **Presentation.** On the skill row, the total change from normal. Beneath it one line per source, in engine order: "Armsman (rank 3): x 1.6", "Fortify One-handed, through Perk Skill Boosts: x 1.25", and under that line the value's own sources, the gauntlets and the potion, from the existing contributions. A conditional entry reads as its condition: "Against undead: x 1.5".
6. **Spell cost is per spell.** Its second argument is the spell, and conditions look at it (Bard Song's magnitude doubling is conditioned on the spell being Bard Song). So cost provenance belongs on the spell's hover in the Magic tab, where the cost already comes from the engine's cost call and so already includes every entry. The school row can carry only what applies to every spell of the school.

**Verified 2026-09-13 (above):** the actor-value functions carry their value's index and the multiplier in two-value data, and read the perk owner's current value.

## Worked example: Adamant's Bard in Nordic Souls

The follower's sheet read "+100 Magicka/s" for Bard Song, and nothing for the cost cut the perk promises. Both traced on 2026-09-13.

- **The Magicka buff** is a hidden Peak Value Modifier with the Recover flag set, magnitude 25 for 600 s on the record. Recover set means the value moves once and moves back when the effect expires, a fortify; clear means it moves every second, a heal or a poison. The sheet had guessed "per second" from the value being a pool; it now reads the flag (commit 88c9501). The 100 on the sheet is 25 doubled by the Bard rank and doubled again on arrival (next point).
- **The cost cut and the doubling** are a perk, Flute Buff Perk (the NPC version), which a SPID file in the Bard add-on hands to every NPC. Its entries: Mod Spell Cost x 0.8 when the player holds Skald and the follower has the song's marker effect; x 0.9 when the player lacks Skald; Mod Incoming Spell Magnitude x 2 when the player holds Skald and the incoming spell is Bard Song. The marker effect is a Script archetype of magnitude zero with no script: it exists so a condition can ask whether the song is on the actor.
- **Why the sheet showed nothing for the cut:** it is not an effect, so the effects table cannot list it, and no value moves, so the skill row cannot either. It shows on the perk's own page under Other Perks, and the Magic tab's cost column, which asks the engine, should already be 20% lower while the song plays (not yet watched in play).

## Bug, on master: Fortify skill values counted twice for a follower who holds the hidden perk

Nordic Souls hands every NPC Perk Skill Boosts (0xCF788): Apothecary's and Thaumaturgy's SPID files both do it. For such a follower the weapon damage figure applies the engine's Mod Attack Damage entry point, which already includes the hidden perk's "one plus a hundredth of One-handed Modifier" entry, and then multiplies the Fortify value in again by hand, guarded by a check that the actor holds that very perk. The guard was written on the assumption that no follower holds it, which made the hand multiply a no-op; in this list it is a double count, and the sheet's "+25% damage" follower is quoted 25% too strong. Found 2026-09-13, not yet fixed.

The fix is to drop the hand multiply: the entry point includes the value when the perk is there, and the value is inert when it is not. Confirm first by logging both figures for that follower. The Modifiers column on the skill row is a separate calculation and reads the values directly; it is right as a statement of the dial, and becomes a line under the entry-point total once the strategy above is built.

## Which actor values matter for a follower

From UESP's actor value index (164 values, `Skyrim_Mod:Actor_Value_Indices`, read 2026-09-13) against what the sheet shows today. "Read by" is UESP's account plus what this project has already measured; a value marked *verify* has not been checked against the executable.

**Already on the sheet.** The eighteen skills (Skills tab, with their Modifier and Power Modifier dials in the Modifiers column, shown only for an actor with the hidden perk that reads them). Health, Magicka, Stamina. Heal, Magicka and Stamina Rate with their Mults (Regen). Speed Mult and Movement Noise Mult (General). Damage Resist as Armor, and the six resistances (Defense). Unarmed Damage. Right and Left Item Charge (the hands). Carry Weight is in the snapshot for the rules but not on the sheet.

**Read by the engine directly, relevant to a follower, not yet shown.** These are the real gap, and none of them is a skill:

| Value | What it does | Where it belongs |
|---|---|---|
| Attack Damage Mult (154) | multiplies every physical hit: weapons, fists, bash. Default 1. Vampire Lord, werewolf, and mods write it | the hands' damage figure; *verify* whether the hit path reads it inside or beside the Mod Attack Damage entry point |
| Melee Damage (34) | flat points on weapon damage. Rare in vanilla | the hands' damage figure |
| Weapon Speed Mult (85), Left Weapon Speed Mult (132) | attack and draw speed; Elemental Fury, perks in the overhauls. Default 0 and yet a multiplier, so 0 and 1 both mean normal (UESP) | the hands |
| Crit Chance (33) | chance of a critical hit with a weapon | the hands |
| Armor Perks (65) | armor rating multiplier, 0.25 for +25%. *Verify* that the Armor figure, which asks the engine's rating entry point, already includes it | Defense |
| Absorb Chance (83) | chance to negate an incoming spell and take its cost as magicka; the Atronach stone, a Breton's Spell Warding (the hidden effect the sheet met on 2026-09-11) | Defense |
| Reflect Damage (163) | chance to reflect incoming melee damage; Reflect Blows | Defense |
| Mass (36) | stagger; hidden | Defense, if at all |
| Shout Recovery Mult (86) | scales the voice cooldown the Shout action waits on | Attack, when the follower has a shout |
| Carry Weight (32), Inventory Weight (31) | an overloaded follower walks | General |
| Combat Health Regen Mult (134) | whether health regenerates in a fight. The player's own ability sets 0.7; *verify* what an NPC has, since it decides what the Regen row means in combat | Regen |
| Bow Stagger Bonus (87) | bow stagger chance; *verify* who reads it for an NPC | the hands, for an archer |
| Ward Power (63) | live only while a ward is up | nowhere; transient |

**Statuses, not modifiers.** Paralysis, Invisibility, Water Breathing, Water Walking, Blindness, Telekinesis, Grabbed, Waiting For Player. Conditions read these; the sheet's status line is where they show, not a modifier list. Waiting For Player is a follower's own value and could feed a condition.

**Behaviour, relevant but not modifiers.** Aggression, Confidence, Assistance, Morality decide whether a follower fights, flees or helps. They belong with the combat style, not with the numbers. Energy and Mood do nothing that matters here.

**Player-only or dead.** Bow Speed Bonus (zoom), Dragon Souls, Dragonrend, the two vendor bypasses, the favor values, Fame, Infamy, Voice Points and Rate, the seven limb Condition values, Night Eye, Detect Life Range, Ignore Crippled Limbs, Jumping Bonus, Shield Perks, Ward Deflection, Grab Actor Offset, the eighteen Skill Advance values, the deprecated slot. Werewolf Perks and Vampire Perks only for a transformed actor. Variable01 to Variable10 are mod-defined and mean whatever the mod says; nothing general can be shown for them.

**Entry points that matter, which are not values at all.** Of the engine's 92, the ones a follower's figures meet: Mod Attack Damage, Mod Power Attack Damage, Mod Bashing Damage, Mod Power Attack Stamina, Calculate My Critical Hit Chance and Damage, Mod Sneak Attack Mult, Mod Target Damage Resistance (armor piercing); Mod Armor Rating, Mod Incoming Damage, Mod Percent Blocked, Mod Shield Deflect Arrow Chance, Mod Incoming Stagger, Mod Target Stagger; Mod Spell Cost, Magnitude and Duration, Mod Incoming Spell Magnitude and Duration, Mod Ward Magicka Absorption Pct, Mod Recovered Health (heals and potions), Mod Shout OK, Mod Poison Dose Count; and the spell-applying ones, Apply Combat Hit Spell, Apply Bashing Spell, Apply Weapon Swing Spell. The rest are the player's: prices, lockpicking, pickpocketing, alchemy, enchanting, telekinesis, activation, bow zoom.

**Where to show them: decided 2026-09-13.** The rule is one sentence. *Wherever a relevant value is shown, it is the value as it applies to the follower at that moment, and hovering the value or its label shows the breakdown of what makes it: base, equipment, enchantments, potions, effects, perks, whatever.* No Other Skills section; each value joins the figure it feeds.

- **Attack.** A General table before Right Hand and Left Hand, with Attack Damage Mult and Melee Damage as they apply to every hit, and the overall shout cooldown modifier (Shout Recovery Mult). Weapon Speed Mult and Left Weapon Speed Mult go into each hand's Speed, which then reads the speed as swung, with the weapon's own speed and the multiplier in the hover. Crit Chance is per hand and per weapon: it goes in the hand table, and the weapon's page, which has Critical Damage and no chance, gets a Critical Chance row.
- **Shouts.** The shout's page has a Cooldown row from the voice's live recovery; its hover gets the breakdown, the word's recovery and the Shout Recovery Mult that scales it. The Mod Shout OK entry point, if any perk uses it, shows there too.
- **Defense.** Spell Absorb after Magic. Reflect after Armor. Mass stays off: it is the stagger comparison's hidden weight and says nothing a player reads.
- **General.** Carrying is already shown; its hover gets the breakdown of Carry Weight, base and effects, against Inventory Weight.
- **Regen and status** stay in their sections on the Character page; nothing moves.
- **Magic.** The cost column and the spell page's Cost row already come from the engine's per-actor cost call, so they are what the follower pays, perks in. The hover on the cost cell gets the breakdown: base cost, the skill curve, each Mod Spell Cost entry that applied (the hidden perk's dial with its sources, a song, a school perk). The Mage rows' Modifiers column stays as the dial with its sources.
- **The hands' Damage** likewise: the row is the engine's figure and the hover lists the skill curve, tempering, each Mod Attack Damage entry that applied in engine order, and Attack Damage Mult.

## Open questions

- Whether the player's walk, hooked in the Nordic Souls process, is the same as on disk there too, and by which plugin. Not needed for followers.
- Whether a perk SPID adds at runtime reaches the process's arrays the same as an authored one. Expected yes: both go through the entry's apply when the process takes the base's perks; the Bard perk's page showing its entries as met says the entries at least evaluate. Measure by comparing our walk's factors with the engine's figure for a SPID-only perk.
- How full ties (same priority and rank) order, if a mod ever ships two that interact. The binary search decides; the self-check will show it.
- Which entry points matter for the sheet beyond attack damage and spell cost: armor rating (already asked of the engine), incoming damage, incoming spell magnitude, power attack stamina, bash damage.
