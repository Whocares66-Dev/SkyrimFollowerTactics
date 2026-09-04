# What Dragon Age's tactics offer, and what fits Skyrim

Research done 2026-09-04 on Dragon Age: Origins, the Advanced Tactics and
EMAT mods, Dragon Age II and Inquisition, and Pillars of Eternity's
behaviour editor, for the question: which of it makes a predictable,
synergistic party in a real-time game read on a half-second tick.

## 1. Origins

**Model.** Rules top to bottom; the first whose condition holds is
attempted; if the action completes the list restarts, if it cannot (no
mana, on cooldown, unreachable) the next row is tried; if nothing fires a
behaviour preset acts (attack whoever is meleeing you, nearest visible,
nothing). Evaluation is event-driven, not ticked. `Self: Any` is a
catch-all evaluated once per combat, which is how "buff at the start" is
written, and a well-known trap.

**Subjects.** Self; Ally, with a sub-pick of the main character, the
controlled character, or each companion by name; Enemy. Enemy conditions
divide into ones that *choose* the target (lowest health, nearest, the one
attacking a party member, the target of a party member, clustered, rank)
and ones that *test* the current target (health, armour type, range,
attack type) and fail with no target.

**Conditions**, the whole list: health and mana thresholds at six steps
each way; lowest and highest health; clustered with 2 to 5; most hated
(threat); nearest visible, by class, by race, by sex; attacking a party
member; target of a party member; has a buff at a range; has status
(paralysed, stunned, sleeping, knocked down, rooted, dazed, slowed,
grabbed, immobilised, charmed, polymorphed, stealthed, dead); armour type
low, medium, high and their unions; being attacked by melee, ranged,
magic and their unions; target using an attack type; at least N enemies
alive, N dead, N allies alive, N wounded, N with curable effects; target at
short, medium, long range; surrounded by N; target at a flank; target rank
critter to elite boss; any; game mode combat or exploration.

**Actions.** Attack; any talent or spell; use item; switch weapon set;
jump to rule N; wait. No movement. The community's ordering rule: buffs at
the top, self-preservation next, reactive rules next, focus fire last, and
never an infinite row above anything.

## 2. Advanced Tactics and EMAT

Advanced Tactics separated `Target` (the current one) from `Enemy` (which
now prefers the current target, to stop switching), added per-companion
subjects, unhid the conditions above, added debuff and curable checks,
"not being attacked by", exhausted allies, summons, and actions for pause,
behaviour change, weapon sets, potion tiers. Its author's motive: vanilla
followers froze mid-cast, stopped attacking, and stalled on dead targets.
EMAT went further: AND-chained conditions, an ability mutex, boolean
variables, 50 to 100 slots, and the verdict that "default AI was so
stupid that you had to put all your effort to keep your characters from
killing themselves". Its fixes are mostly implicit guards: no area spell
with an ally inside, no crowd control on the controlled, no heal at full.

## 3. Successors

Dragon Age II kept the model and adopted the mod's ideas: party-wide
counts, "use current condition for next tactic" as a poor man's AND, skip.
Inquisition dropped the list for per-ability toggles and a target choice.
Pillars of Eternity II's editor is the cleanest design: a script is a set
of ANDed conditions, each negatable, an action list where the first usable
wins, a target type, a **prioritize-by axis** (nearest, farthest, lowest
health, lowest armour, most enemies in the area) separate from the
condition, and a per-script cooldown. Its modders' first request was "is
casting".

## 4. What fits Skyrim

**Already here or cheap.** Health, magicka, stamina thresholds; lowest and
highest; nearest by distance; is casting, is fleeing, staggered, paralysed
and the rest as Status; armour and resistance bands; combat start, during,
end; attacked by, from the hit events; the other followers by name. Count
of enemies is CountAtLeast. "Target of the player" and "attacking the
player" are one read each off the enemy views (`isAttackingPlayer` is
carried; "target of" is the player's own combat target) and are the two
most valuable for a party: focus fire is `Enemy: target of the player`
at the bottom of the list, and peeling is `Enemy: attacking the healer`.

**Worth borrowing.**
- The **prioritize-by axis** from Pillars: "Enemy casting, nearest to the
  player" reads better and costs less than a cross-product of conditions.
  Our Lowest and Highest are the first two values of that axis.
- **Implicit guards in the engine**, not the list: no area spell with an
  ally in it, no heal on a full ally, no crowd control on the controlled.
  The player will not write them, and Advanced Tactics had to.
- **Hysteresis for weapon switching**: switch in at one range and out at
  another, or the bow and the sword dance at the half-second boundary.
- A **per-rule cooldown** as Pillars has it, for rules that fire on a
  standing condition and should not fire again for a while.

**Translates badly.** Jump-to-rule and "use the current condition for the
next" are workarounds for one condition per rule; if ever needed, a small
AND list is the honest version. Flank positions are noise at half a
second. Threat is not exposed. Movement actions: Origins had none and the
package AI owns pathing here; offer stances (hold, follow, keep range) as
packages if at all.

## Sources

dragonage.fandom.com Tactics (Origins) and Conditions;
dragonage.miraheze.org Conditions, Tactics (Dragon Age II); Nexus Dragon
Age mods 181 (Advanced Tactics), 4096 (EMAT), 2206 (Clustered fix); the
dragon-age LiveJournal tactics guides; screenrant on Inquisition; the
Pillars of Eternity wiki AI behaviors; Sombrero's Steam guide to
Deadfire's editor; Nexus Pillars 2 mod 88.
