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

**The order is not yet known.** The functions do not commute (add after multiply is not multiply after add), so the order the entries are applied in decides the number. The Creation Kit wiki says an entry's Priority "controls the order of operations when two perk entries affect the same value" and that "a higher priority takes precedence", which does not say whether higher runs first or last, nor whether the sort happens for an NPC at all or only in the player's per-entry-point arrays. To find out: disassemble `Actor::ForEachPerkEntry` (vtable slot 0x100) and `PlayerCharacter`'s override, and `BGSEntryPointPerkEntry::ApplyPerkEntry`, which is where a sorted insert would be (`tools/disasm.py`, `docs/CLAUDE.md` "Reading the executable"). Until then the honest presentation is a product in engine order, not a sum, and the self-check below catches a wrong guess.

**Where an NPC's perks live.** On the actor base (`TESNPC`'s perk rank array), which is also where SPID puts a distributed perk, and which `Actor::HasPerk` reads. The player's are on the player character in its own arrays.

## Strategy: total from the engine, provenance from our own walk, and a check between them

1. **The total** comes from the engine's entry point call, as the weapon damage figure already does (`WeaponDamage` in `src/game/Sensors.cpp`). It is the number a swing would use, with every perk the engine would count and none it would not.
2. **The provenance** comes from our own walk of the same entries: `ForEachPerkEntry` on the actor for the entry point, evaluating each entry's conditions with the same arguments the total used, and formatting each surviving entry as one factor with its perk's name. The perk page's entry formatter (`EntryRow`) already words the functions; this needs the numbers.
3. **Only what applies is in the total.** An entry whose conditions fail against the arguments in hand is listed as conditional, with the condition named, and left out of the figure. That includes entries that read a target: outside a fight there is none, and the figure is the one the player's own inventory shows, which has no target either. When a target is known (a rule's evaluation, a fight) the same walk can be run with it and the figure changes, which the sheet should say.
4. **The self-check.** Our factors multiplied together in the order we walked them must equal the engine's figure. When they do not, log both with the entries, as the value note logs a value its listed sources do not explain (`LogUnappliedSources`). This is how a wrong ordering guess, or a function we format wrongly, shows itself.
5. **Presentation.** On the skill row, the total change from normal. Beneath it one line per source, in engine order: "Armsman (rank 3): x 1.6", "Fortify One-handed, through Perk Skill Boosts: x 1.25", and under that line the value's own sources, the gauntlets and the potion, from the existing contributions. A conditional entry reads as its condition: "Against undead: x 1.5".
6. **Spell cost is per spell.** Its second argument is the spell, and conditions look at it (Bard Song's magnitude doubling is conditioned on the spell being Bard Song). So cost provenance belongs on the spell's hover in the Magic tab, where the cost already comes from the engine's cost call and so already includes every entry. The school row can carry only what applies to every spell of the school.

**To verify before building:** where an entry stores the actor value its actor-value functions read. The one-value function data holds a single float; the "multiply by one plus a share of the value" form needs both a value and a multiplier, so it is either a two-value record or the value is elsewhere. Check the headers, then a live entry.

## Worked example: Adamant's Bard in Nordic Souls

The follower's sheet read "+100 Magicka/s" for Bard Song, and nothing for the cost cut the perk promises. Both traced on 2026-09-13.

- **The Magicka buff** is a hidden Peak Value Modifier with the Recover flag set, magnitude 25 for 600 s on the record. Recover set means the value moves once and moves back when the effect expires, a fortify; clear means it moves every second, a heal or a poison. The sheet had guessed "per second" from the value being a pool; it now reads the flag (commit 88c9501). The 100 on the sheet is 25 doubled by the Bard rank and doubled again on arrival (next point).
- **The cost cut and the doubling** are a perk, Flute Buff Perk (the NPC version), which a SPID file in the Bard add-on hands to every NPC. Its entries: Mod Spell Cost x 0.8 when the player holds Skald and the follower has the song's marker effect; x 0.9 when the player lacks Skald; Mod Incoming Spell Magnitude x 2 when the player holds Skald and the incoming spell is Bard Song. The marker effect is a Script archetype of magnitude zero with no script: it exists so a condition can ask whether the song is on the actor.
- **Why the sheet showed nothing for the cut:** it is not an effect, so the effects table cannot list it, and no value moves, so the skill row cannot either. It shows on the perk's own page under Other Perks, and the Magic tab's cost column, which asks the engine, should already be 20% lower while the song plays (not yet watched in play).

## Bug, on master: Fortify skill values counted twice for a follower who holds the hidden perk

Nordic Souls hands every NPC Perk Skill Boosts (0xCF788): Apothecary's and Thaumaturgy's SPID files both do it. For such a follower the weapon damage figure applies the engine's Mod Attack Damage entry point, which already includes the hidden perk's "one plus a hundredth of One-handed Modifier" entry, and then multiplies the Fortify value in again by hand, guarded by a check that the actor holds that very perk. The guard was written on the assumption that no follower holds it, which made the hand multiply a no-op; in this list it is a double count, and the sheet's "+25% damage" follower is quoted 25% too strong. Found 2026-09-13, not yet fixed.

The fix is to drop the hand multiply: the entry point includes the value when the perk is there, and the value is inert when it is not. Confirm first by logging both figures for that follower. The Modifiers column on the skill row is a separate calculation and reads the values directly; it is right as a statement of the dial, and becomes a line under the entry-point total once the strategy above is built.

## Open questions

- The order the engine applies entries in, for the player and for an NPC (above).
- Whether `ForEachPerkEntry` on an NPC visits perks SPID added at runtime the same as authored ones. Expected yes, since both sit on the base; measure it.
- Which entry points matter for the sheet beyond attack damage and spell cost: armor rating (already asked of the engine), incoming damage, incoming spell magnitude, power attack stamina, bash damage.
