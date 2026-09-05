# Actions: heal, buff, debuff, shouts, transformations, dual casting, attack

What the engine lets a follower do on request, from research done
2026-09-04 (sources at the end). The existing actions are potions, a cast
through a UseMagic package, and the equip pins. This is about what the
next ones can be built on, and what they cannot.

## 1. Heal, buff and debuff on someone else

**Vanilla combat AI never casts a beneficial spell on anyone but itself.**
The caster classes in its list are offensive, restore-self, ward, armour,
summon, bound weapon, cloak, invisibility, light, disarm, paralyse,
reanimate, stagger, target-effect and scripted; `Restore` is self only.
Follower-healer mods (Be With Healer, MSFF, the 3DNPC healers) all do it
the way we already cast: a package that overrides combat with the spell
and a target input.

So **heal / buff an ally is our CastSpell with a target**, and the pieces
are in place: the package's Target input takes a single reference, the
lease repoints it per cast, and the step already carries a target actor.
What is missing is (a) Execute honouring the step's target for a targeted
spell rather than always the follower's combat target, and (b) a way to
choose the target in the editor. Both are small. Aimed delivery (Healing
Hands, a stream) at a moving ally is unverified; Heal Other and Grand
Healing are fire-and-forget or area and safer to start with. Healing Hands
on a follower can aggro them on some records; Heal Other does not.

**Debuffs do not exist as castable spells in vanilla.** Weakness to
Fire/Frost/Shock/Magic/Poison are alchemy effects, poisons, abilities and
one shout (Marked for Death); no spell record references them. Apocalypse,
Odin and Mysticism add castable ones. So "debuff enemy" is: poisons on the
weapon (a Use Poison action, which NPCsUsePotions already does for NPCs),
Marked for Death if the follower can shout, or a spell mod. Buffing an
ally's armour or resistance is the same story: vanilla's Stoneflesh line
and the resist spells are self-only; targeted versions need a mod's
records or ones of our own in the ESP.

## 2. Shouts

**Any NPC with the shout in their spell list can shout**; the voice type
only decides whether the words are audible. Ulfric and the Greybeards are
ordinary actors with a TESShout listed; Shouty People distributes seven
shouts through SPID and they are used. The combat AI scores a shout entry
by `shoutScoreMult` and equips it when the voice recovery time is below a
game setting.

**The cooldown is per actor**, in the high process data: `voiceRecoveryTime`
and `voiceTimeElapsed`, read by `Actor::GetVoiceRecoveryTime()`. Each word
has its own recovery time on the shout record, scaled by the
ShoutRecoveryMult actor value. CommonLib has no setter; the field can be
written.

**On demand:** the engine has a `Shout` package procedure (Shout, Target,
HoldWhenBlocked), used by the Greybeard training; it is the shout twin of
UseMagic and splices into the alias the same way. `EquipShout` alone only
selects the power; the package or the AI fires it. Untested whether the
procedure honours the recovery time.

So a **Shout action** is: an ESP `Shout` package pool beside the UseMagic
one, a lease per cast, a snapshot field for the recovery time so the rule
can report "cooling down" rather than fire into it, and the follower must
know the shout (a `Learn shout` action, or the player teaches it).

## 3. Transformations

Werewolf and Vampire Lord are **powers whose effect scripts support
NPCs**: the effect always casts the transform-visual spell, which on a
non-player does `SetRace(WerewolfRace)` / `SetRace(DLC1VampireLordRace)`
after the animation event. Vanilla uses it on Sinding, Aela and Harkon.

What an NPC does **not** get is everything the player quest handles:
duration, the shift back, feeding, re-equipping gear afterwards. That is
ours to do: `Actor::SwitchRace` back to the original race on a rule or a
timer, and put the outfit back on (the pin book can hold it). Vampire Lord
combat on an NPC is the known problem, magic mode "extremely buggy with
NPCs"; werewolf mods (Beastblood, The Beast Within, Growl) are all script
and all cope. A **Transform action** is therefore: cast the power at the
follower through the instant caster or a UseMagic package, remember the
race, and a paired **Revert** with the outfit restore. Plausible; test
werewolf first.

## 4. Dual casting

**NPCs can dual-cast; vanilla mage bosses do.** The combat behaviour tree
has a PrepareDualCast node, and 139 vanilla NPC records carry the school
dual-casting perks. The gate is the perk entry point CanDualCastSpell,
which the school's Dual Casting perk sets, so a follower needs that perk
(`Actor::AddPerk`). No combat-style flag is involved.

Through **our package**, the UseMagic procedure has a DualCast boolean
input, false on our eight slots today; flip it per lease as the spell and
target are repointed. Whether the perk is still required through the
package is unverified. Direct API: dual cast is state on the hand caster
(`ActorMagicCaster::SetDualCasting`), not a cast argument. Watch for po3's
Dual Casting Fix: scripted casts clear the dual state, and our faction-rank
abilities might too.

## 5. What to build, in order

1. **Cast on a chosen target**: Execute takes the step's target; the
   Then cascade's first level names it (self, the ally or enemy the
   condition matched, the player, a named follower, the target, the
   attacker), as the If cascade's names the subject. Heal the player, heal the hurt ally, buff the one about
   to engage. Vanilla spells only.
2. **Use poison** on the weapon: NPCsUsePotions' path, follower-aware.
3. **Shout**: the package pool, the recovery time in the snapshot.
4. **Dual cast** as a flag on a cast action, needing the perk.
5. **Transform / Revert**, werewolf first.

## 6. Attack: focus aggression on one enemy

Research 2026-09-04. **The engine has no "attack" verb.** An actor in combat attacks whoever the combat controller's *target selector* has chosen; the behaviour tree and the inventory scoring then decide how (swing, shoot, cast). So the action is "focus on this enemy", and once the target changes the follower's own scoring picks the weapon or spell for it. Nothing here needs to branch by class.

**What exists, verified against the 3.7.0 headers in this build tree:** `CombatController` has `targetHandle`, `previousTargetHandle`, a `targetSelectors` array and a `currentTargetSelector`; `Actor` mirrors the target as `currentCombatTarget`. Two selector classes are in the RTTI and vtable tables, `CombatTargetSelectorStandard` (the AI's own choice) and `CombatTargetSelectorFixed` (the engine's own "this one, regardless"). Neither has a mapped layout, so using the fixed one means reverse engineering. The `CombatGroup` holds one `targets` list for the whole group with a threat value and attacker count per target; followers and the player share a group.

**What the game files say (Skyrim.esm through houseCARL):** no game setting tunes target selection. The 96 `*Combat*` settings cover stealth points, regen, dialogue timers and radii; the only threat-related one is `fCombatThreatRatioUpdateTime` = 5 s. The INIs (game default, My Games, MO2 profiles) carry no target or selector settings either. So the standard selector's weights are hardcoded and can only be measured.

**Papyrus `StartCombat(target)` is not the tool.** The CK wiki says only "attempts to get this actor to initiate combat with the target". All 155 vanilla scripts that call it start a fight (Jorrvaskr brawl, Vilkas training, Cicero, the Silver Hand ambush); none retargets an actor already fighting, and that case is undocumented. A one-console-command test would settle what it does mid-combat.

**Bethesda's own "keep attacking this one" is a UseWeapon combat override.** Karliah and Brynjolf in Blindsighted run `TG08B*UseWeaponCombatOverride`: template `UseWeaponAlreadyHeld`, Target to Attack = an alias, Never End = true, `InterruptOverride = Combat`. The template's inputs are Weapon Type, Target to Attack, Always Hit, Do No Damage, Hold when Blocked, Never End, barrage counts and pauses, Max Time spent Attacking, Always Power Attack, Headtrack Target. The CK wiki adds: the actor attacks only "if the actor has a weapon of the specified Weapon type", and with an object list only the first target counts. So it is weapon-bound: a caster would need UseMagic instead, and the package drives the attacks rather than the combat style. Our eight `FT_CastSlot` records are the UseMagic twin (template `UseMagic`, flag IgnoreCombat, faction-rank condition) spliced at runtime to the front of the follower alias's combat-override form list, so a UseWeapon pool would splice the same way.

**Routes, in the order to try them:**

1. **Assert the target directly.** Each tick while a focus lease is held, write `targetHandle` and `currentCombatTarget` to the enemy and measure whether the standard selector snaps back before the next 500 ms tick. Twenty lines. If it holds, the whole AI stays in charge of everything but the target, which is the "bias, don't puppet" principle exactly.
2. **Hook the standard selector.** A vtable write like the score hook, returning our target while the lease lives. Cleanest, but the virtual's index and signature are unknown. Unverified.
3. **A UseWeapon combat-override pool**, spliced like the cast slots, with UseMagic for casters. Heaviest, class-branching, and it puppets.

Availability: the target is alive, hostile and in the follower's combat group. Release on death, on combat end, or after a timeout, like a cast lease.

**Built 2026-09-04 as the `Target` action, route 1.** `IF Ally: Attacked by Ranged THEN Attacker: Target` is the shape. Core: the action is available only in combat, only when its target is one of the snapshot's enemies, and not when they are already the follower's current target, so a standing rule falls through instead of re-firing; its cooldown is 2 s keyed by the action alone, so two Target rules cannot flick the follower between two enemies on consecutive ticks. Game: `Execute` writes the combat controller's `targetHandle`, `previousTargetHandle` and `cachedTarget`, and the actor's `currentCombatTarget`, and logs the old and new target. There is no separate instrumentation for the open question: if the standard selector lets the choice stand the rule reports "already fighting them" on the next tick, and if it snaps back the rule fires again after its cooldown, so the log answers it. Not yet measured in play. Ranged is a damage kind of its own: a physical hit whose event carries a projectile; a blow without one is Melee.

## Sources

CK wiki UseMagic (Procedure), UseWeapon (Procedure), Shout (Procedure), StartCombat - Actor, GetCombatTarget - Actor, IsCombatTarget, Package Flags, Combat
Style, Magic Effect; UESP Shouts, Magic Overview, Weakness to Fire;
CommonLibSSE-NG ActorMagicCaster and MagicCaster; powerof3/DualCastingFix;
Nexus: Shouty People, Heroes of Yore, Shout Recovery Utilities, Vampire
Lord Traveler, Vampire Bloodline, Beastblood, The Beast Within, Growl, All
NPC use Healing, Be With Healer, MSFF, Apocalypse, Odin, Mysticism,
Apocalypse Spells For NPCs; 3DNPC's Combat AI article; vanilla Papyrus
sources (WerewolfChangeEffectScript, DLC1VampireTurnScript,
DLC1VampireChangeEffectScript); Skyrim.esm records through houseCARL (the UseWeapon, UseWeaponAlreadyHeld, UseWeaponMultiTarget and UseMagic templates, the TG08B combat overrides, dunCGUseWeaponArcher, the Combat game settings); CommonLibSSE-NG CombatController, CombatGroup, Offsets_RTTI and Offsets_VTABLE.
