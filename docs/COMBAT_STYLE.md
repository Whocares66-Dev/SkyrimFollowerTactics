# Combat styles

What the combat AI is tuned by, and what the panel does with it. The field
descriptions are the Creation Kit wiki's ("Combat Style"), kept here because
that page refuses automated fetches. The value ranges are measured, not
quoted: every style in this load order (163 of them, 2026-09-03) read through
houseCARL.

## What a style is

A `CSTY` record is the combat AI's tuning, not its logic. The logic is engine
code; the style feeds it numbers and flags. An actor uses the style on her
base record (`TESNPC::combatStyle`); in a fight her `CombatController` holds
a pointer to it as well. Vanilla styles are shared by every actor of a kind
(csHumanMagic by every mage), so the panel never edits one in place: a
follower who needs a change gets a **runtime copy** (`CreateDuplicateForm`,
a 0xFF FormID) assigned to her record and her live controller. The copy is
not written to the save -- created forms are saved only for weapons, armour,
potions, enchantments and references, and the NPC change form does not
carry the combat style -- so it and anything pointing at it are gone on
reload, which is what makes it safe to uninstall over.

## Two scales

| family | fields | range seen | neutral |
|---|---|---|---|
| chances and movement | offensive, defensive, group offensive, avoid threat, special attack, circle, fallback, flank distance, stalk time, strafe | 0 to 1 | -- |
| score and attack multipliers | the six equipment scores; attack staggered, power attack staggered, power attack blocking, bash, bash recoiled, bash attacking, bash power attacking | 0 to 10 | 1 |

The panel shows each value as `x / 1` or `x / 10` accordingly.

Reference points: csHumanMagic (Marcurio) has magic score 4.05, melee 0.76,
offensive 0.65, defensive 0.5, Dueling. csHumanMissile has ranged 3.2, melee
0.83. Of the 163 styles, 113 are Dueling, 27 Dueling with dual wielding, 11
Flanking, 2 Flanking with dual wielding.

## The fields (Creation Kit wiki)

**General**

- **Offensive Mult** -- works with Defensive. The higher, the more likely a
  character attacks, the more often, and the more often with a power attack.
- **Defensive Mult** -- the higher, the more a character blocks, the longer
  the block is held, and the more they bash if they can.
- **Group Offensive Mult** -- overrides Offensive in a group: the more actors
  attacking one target, the less offensive each is, by this mult. Higher
  keeps them offensive in groups.
- **Avoid Threat Chance** -- not used, or use unknown to the author.
- **Equipment Score Mults** -- the higher, the more likely the actor uses
  that kind of equipment. *Multiplied into the damage output of the attack*:
  a weak melee attack against strong spells needs a very high melee mult
  before the actor prefers melee. This is a comparison of weighted damage,
  not a percentage of the time.

**Melee**

- **Attack Staggered** -- the higher, the more likely an attack on a target
  in a stagger state.
- **Power Attack Staggered** -- the same, with a power attack.
- **Power Attack Blocking** -- the higher, the more likely a power attack on
  a blocking target, to break the block.
- **Special Attack** -- not used, or use unknown to the author.
- **Bash** -- the higher, the more likely a bash (shield, or an attack
  flagged as a bash), which can stagger and interrupt.
- **Bash Recoiled** -- against a target recoiling from its own blocked
  attack.
- **Bash Attack** -- against a target mid-attack.
- **Bash Power Attack** -- against a target mid-power-attack.
- **Allow Dual Wielding** (flag) -- lets an NPC dual wield. Works only on
  NPCs with dual-wielding animations, currently humanoids. An actor whose
  style forbids it takes a left-hand weapon straight off again.

**Close range** -- only one of the two modes is active, by flag:

- **Dueling**: **Circle Mult** (how much the actor circles the target rather
  than standing still), **Fallback Mult** (chance to back off).
- **Flanking**: **Flank Distance** (distance kept while flanking), **Stalk
  Time** (time spent flanking before attacking).

**Long range**

- **Strafe Mult** -- how much the actor strafes to dodge projectiles when
  out of melee range.

**Flight** -- dragons only; irrelevant to a follower and not shown.

## How the panel uses it

- The Combat Style tab, before Tactics, shows the live style with these
  descriptions as hover text and only the active close-range pair. The two
  unused fields and the flight (dragon) fields are left off.
- A left-hand weapon pin gives her a copy with dual wielding allowed
  (`AllowDualWield` in `Tactics.cpp`). Pins to a hand stand down in combat;
  the style is the lever that makes her own choice match them.
- Next: a dropdown of named styles (wizard, spellsword, berserker, archer)
  as tuned copies -- the `SetCombatStyle` palette `PLAN.md` 3.8 anticipates.
  Whether the AI reads the score multipliers live or only at combat start
  is unverified and decides whether a change needs a recompute per pin or
  merely per fight.

## Not exposed

The AI's decision logic itself is not exposed. Mods such as Puppeteer and
Combat AI Uncapped replace it with SKSE hooks into engine routines, which is
version-specific, crash-prone work outside this project's thin game layer.
The style is the sanctioned lever.
