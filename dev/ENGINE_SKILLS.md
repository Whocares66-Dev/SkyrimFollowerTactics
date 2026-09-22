# How the engine levels the player, and how companions learn the same way

Read 2026-09-21 from the unpacked executables in `C:\Modding\SkyrimVersions` (1.5.97, 1.6.1170, 1.7.104) with `tools/disasm.py` and `tools/rtti.py`, and the load order's game settings through houseCARL. As with perks and spells ([ENGINE_PERKS.md](ENGINE_PERKS.md), [ENGINE_SPELLS.md](ENGINE_SPELLS.md)), everything here was read from the code; none of it has been seen in play. The plugin's side is `src/progression/game/Learning.cpp` (the hooks), `src/progression/game/Rules.cpp` (the numbers) and `src/progression/core/Levelling.cpp` (the formulas). IDs are AE (1.6.1170) unless marked.

## Why

The aim is that a companion levels exactly as the player does: a skill rises from use, skill-ups make levels, and a mod that retunes the player's levelling retunes companions the same way. So the rules have to be the engine's, and their numbers have to be read from the game while it runs, not copied into the plugin.

## The player's levelling

`PlayerCharacter::UseSkill` (40488) passes any use worth more than 0 to the player's skill record (`player+0x9B8`, CommonLib's `PlayerSkills`), through 41561:

| Step | Rule | Numbers from |
|---|---|---|
| Skill XP from one use | points × use multiplier + use offset | The skill's own record (AVSK), read by 27244 |
| *Mod Skill Use* perks (the Guardian Stones) | the perk entry point 0x16, evaluated for the player | The player's perks; the condition *IsAdvanceSkill* reads the skill being advanced from a field on the player |
| XP to the next skill level | improve multiplier × level ^ `fSkillUseCurve` + improve offset | The skill's record; `fSkillUseCurve` is 1.95 in Skyrim.esm (the executable's own default is 1.25) |
| Character XP per skill-up | new level × `fXPPerSkillRank` | 1.0 (no plugin sets it) |
| XP to the next character level | `fXPLevelUpBase` + `fXPLevelUpMult` × level (41560, 41567) | 75 and 25, from Skyrim.esm |
| A level-up brings | `iAVDhmsLevelUp` to health, magicka or stamina, and a perk point | 10 |
| Skill floor | `iAVDSkillStart`; Legendary resets to `fLegendarySkillResetValue` | 15 and 15 (executable defaults) |
| Skill cap | 100 | A constant in 41561 (ID 186355) |

Other paths into the same record skip `UseSkill`: skill books (17842), trainers (52667), the console's `IncPCS` and Papyrus `IncrementSkill`, all through 40489 → 41562 → 41561.

**Which of these can be called for a companion.** Only two:
- **27244** takes a skill and returns its four usage values. It's called directly (SE 26576, an exact match).
- **26423**, the bash formula, takes damage and whether there's a shield (SE 25857).

Every other formula is written inline in a function tied to the player. For example, 41573, "the XP to this skill's next level", reads the player's own skill level rather than taking one. So the two one-line formulas (the skill threshold and the level threshold) are copied, and fed the game's values at the moment they're needed: on each use, the settings from `GameSettingCollection` and that skill's values from 27244; on each action from the panel, and when the panel draws, the same. Nothing is kept between. A mod that changes a setting or a skill's record reaches companions at their next use. A mod that rewrites the code of those player functions doesn't. *Mod Skill Use* perks aren't honoured for companions; vanilla gives followers none.

Assigned attribute points keep the value they had when assigned, as the player's level-ups do, so reconciling them needs no setting.

## Where a companion's skill use comes from

The engine reports skill use through the virtual `Actor::UseSkill`, vtable slot 0xF7. Character's version, which every NPC uses, is a bare `ret 0` (37647). A survey of the 23 functions that call it through the vtable found two kinds.

**Magic** is reported for any caster, so replacing Character's slot catches a companion's:

| Caller | What |
|---|---|
| 34100 | Value modifiers and their kin (Destruction damage, Restoration heals, wards, Alteration armour spells, Detect Life, Telekinesis): once per effect, scaled by the share of magnitude applied. Wards and armour spells only count in combat |
| 34526 | Fire-and-forget spells that land an effect: Illusion, Paralysis, Soul Trap |
| 34143 | Concentration spells, every frame × the frame's time |
| 34243 | Cloaks, per actor they touch |
| 38799, 34229 | Summons and reanimates, and bound weapons, once in combat |
| 34412 | Light spells that hit nothing |

Scrolls, staffs, enchantments, powers and shouts never give skill XP (`SpellItem::GetSkillUsageData`, 11495, says no for them), for the player or anyone.

**Everything else is the player's alone.** It's either guarded by a check against the player or called on the player singleton:
- One-Handed, Two-Handed, Archery, bash, Block and both armour skills are worked out in the hit handler (38627), in two blocks. One is guarded by *attacker is the player*, the other by *victim is the player*.
- Sneak (38586, 40593, 40594), and all the crafting, thieving and speech uses, are player-only.

**The combat skills are mirrored.** Right after those two blocks, 38627 hands the finished `HitData` to the victim's processing (38586). That call, which hit mods commonly hook, is rewritten through SKSE's trampoline:

| Build | Site | Callee |
|---|---|---|
| 1.6.1170, 1.7.104 | 38627 + 0x4A8 | 38586 |
| 1.5.97 | 37673 + 0x3C0 | 37633 |

The hook works out the same uses, the same way, for a companion:
- **Attacking:** a blow that landed for damage on a living, non-child target. The skill is `HitData.skill`. The points are the weapon's base damage, or for a bash, the engine's own 26423 (SE 25857) with whether they hold a shield (38577, SE 37624).
- **Hit and blocking:** Block, by (physical − total damage) × `fWeaponBlockSkillUseMult` + `fWeaponBlockSkillUseBase`.
- **Hit otherwise:** Heavy or Light Armor. The pick is weighted by worn, non-clothing armour with a rating above 0, the body piece counting twice and the shield not at all, against a roll of 0–5, so a half-dressed fighter trains less. The points are the physical damage. The race on their base record says which slots are the shield and the body (`shieldObject`, `bodyObject`: 15695 and 15696, through 19630 and 19631), and the mirror asks the same race. SE puts this pick in its own function (37589, reading the same two slots through 15519 and 15518); the mirror follows it on both.

Every use found is queued to the game thread; nothing in the hooks changes an actor. Both hooks can run on the engine's AI worker threads (38586 itself begins by deferring work that isn't its thread's), and do there only what the engine's own player blocks do: read the actors, the hit and a setting.

Two limits the engine sets, and the mirror keeps:
- **Hits between combat allies** never reach the victim call: 38627 skips it when an NPC hits someone in their own combat group. A companion hit by an in-combat ally learns nothing from it. The player's own blocks run before that check, so the player does.
- **Sneak** is trained only for the player: its one use in a hit (in 38586) goes to the player singleton. A companion's Sneak moves only through the reassigning pool.

The victim call is only rewritten while it is still a plain call (`E8`): if another mod has changed it, the hook stands down and companions learn from magic alone, said in the log. A use still queued when a game is loaded is dropped: each carries the game it was heard in.

## The level

A companion's level is the greater of:
- **the engine's own**, which is your level × the record's multiplier, held between its minimum and maximum (`TESActorBaseData::GetLevel`, 14384; the table of vanilla followers is [research/follower-levels.md](research/follower-levels.md));
- **the level their learning reaches**: their character XP added to the XP of the engine's level, on the player's curve, at most 5 levels above yours.

The engine's level only moves when the player's does, so the player's level-up event (`LevelIncrease`) is when a companion's level is looked at again, besides after their own skill-ups. So a follower the engine caps (Onmund at 30) keeps growing past the cap by what they learn. One who levels with you (Serana, ×1.0 to 50) gets up to +5 from learning. One the engine puts above you (Erandur, ×1.5) is never pulled down.

## Points and reassigning

- **At level N** a companion has what a player at N would have had: N−1 perk points and N−1 attribute points, less what they already hold. Held perks are the skill-tree perk ranks they hold, their own or bought. Held attributes are what their own health, magicka and stamina already carry above their race's starting values, divided by `iAVDhmsLevelUp`.
- **Skills rise only by use,** and never read past 100: when the engine raises their own value later, the learned levels above 100 wait, unread (`WithLearned`).
- **Reassigning:**
  - `−` returns a level to a pool at what it's worth (level × `fXPPerSkillRank`, as a skill-up pays);
  - `+` buys a level from the pool at the same rate;
  - Reset takes a skill to its floor (`iAVDSkillStart` plus the race's bonus to it) and returns the perks bought in its tree, as Legendary does, for free;
  - a level a bought perk needs can't be taken back with `−`.

## Read, not written (2026-09-22)

What a companion has learned and the attribute points assigned are never written to the actor. Character's `GetBaseActorValue` is replaced (`src/progression/game/ValueView.cpp`): for a managed companion's skill it answers the engine's base with the learned levels on top, held to the skill cap (`WithLearned`, core, tested); for health, magicka and stamina, the base with the points' worth on top. Every other actor, and every other value, is the engine's answer untouched.

| What | AE 1.6.1170 | SE 1.5.97 | How it reads the base |
|---|---|---|---|
| Character's ActorValueOwner part | vtable 207896, 0xB8 into the actor | vtable 261402, 0xB0 | Index 5 of CommonLib's `VTABLE_Character`; the offset is `Actor::AsActorValueOwner`'s (0xB0 before 1.6.629, 0xB8 from it), and the executables' RTTI agrees |
| `GetBaseActorValue`, slot 3 | 38464 | 37519 | The actor's own base storage (`+0x150`), else the record's value through the NPC's own value interface |
| `GetPermanentActorValue`, slot 2 | 38463 → 38484 | 37518 → 37535 | 38484 and 37535 call slot 3 through the vtable (`call [rax+0x18]` on the `+0xB8` part), then add the permanent modifier |
| `GetActorValue`, slot 1 | 38462 | | Calls slot 3 through the vtable, then adds the modifiers |

So one slot changes all three reads, and every reader that asks through the actor's value interface -- a perk's `GetBaseActorValue` condition, the damage and cost formulas, the menus, Tactics' own sheet -- sees what they learned. Progression's own reckoning asks the original for the engine's base (`EngineBase`) and adds the ledger itself.

What this buys: nothing of it is in the save, so loading without Progression, or turning progression off, has the follower as their record makes them at once, with no withdrawing and no uninstall step; and the engine raising their own values as they level with the player moves only the base under what they learned. The ledger no longer records what was applied.

One guard: points assigned to health hold the wounds they cover. Turning progression off takes them away at once, so a follower whose health would be at or below nothing without them is left at 1 as they are released.

## Not verified

- **Readers that bypass the interface.** A reader that goes to the actor's value storage directly, rather than through the ActorValueOwner vtable, would see the engine's value without what they learned. None was found in the three getters; the engine's own recalculation of an NPC's values on levelling is not yet read. If it reads a skill through the interface and writes it back as the base, the learned levels would be written into the actor, which the first session should check (`getavinfo` before and after the player levels, with a companion who levels with the player).
- **That the hooks run.** Settings shows the uses heard: from magic, from blows landed, from hits taken. After a fight with a companion who swings a sword and casts, all three should have moved.
- **The rates.** Followers fight all the time and take a lot of hits. By the player's own rules their armour and Block skills may climb faster than a player's, until the curve slows them.
- **Concentration spells** report every frame, and each report is a task queued to the game thread. It's cheap per task, but not measured.
- **Attribute points from their own values.** An NPC's health, magicka and stamina come from their class, not from level-up choices, so "what they already carry" may give too many points or none. The first session should log a few followers' numbers.

## Reproducing

```powershell
python tools/disasm.py --version 1.6.1170 40488                     # the player's UseSkill
python tools/disasm.py --version 1.6.1170 41561 0x800               # use -> skill XP -> skill-up -> character XP
python tools/disasm.py --version 1.6.1170 --vtable 207886 248       # Character slot 0xF7: 37647, a ret
python tools/disasm.py --version 1.6.1170 38627 0x800               # the hit handler; the player blocks at +0x206..+0x466
python tools/disasm.py --version 1.5.97 37673 0x800                 # SE's, the victim call at +0x3C0
python tools/disasm.py --version 1.6.1170 27244                     # a skill's usage values
python tools/disasm.py --version 1.6.1170 38484                     # the permanent value: the base through slot 3
python tools/disasm.py --version 1.5.97 37535                       # SE's
```
