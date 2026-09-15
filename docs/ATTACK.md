# Blows through a UseWeapon package

**Status: an idea, not built (2026-09-14); the procedure read from the running 1.6.1170 executable on 2026-09-15, nothing tried in play.** Power Attack, Bash and Power Bash are sent today as one animation event each, with `NotifyAnimationGraph` (`src/game/Actions.cpp`); their design and first measurements are `docs/ACTIONS.md` section 6. This note is the case for trying Bethesda's own route instead: a UseWeapon package on the follower's stack for the length of a lease, the way casts already go through UseMagic (`docs/MAGIC.md`).

**The short answer.** A UseWeapon package can make the follower swing and power attack, and it cannot make them bash: the procedure reads the actor's attack data and skips every entry flagged Bash Attack, and no input changes that. Bash and Power Bash stay animation events, or need a route of their own.

## Why

The follower's combat AI drives the same animation graph, so an event sent from our tick usually arrives while a swing, a block or a recovery is in progress, and is turned away. Measured 2026-09-09: 3 of 11 blows landed. Seen again 2026-09-14 in Nordic Souls: a Bash rule on Ghorbash failed seven times in 34 s, five "already mid-swing" and two "the weapon is not drawn", each a `rule.actionFailed` warning.

Each of those also spent the action's cooldown. The core stamps the cooldown when it decides, and the refusal is found only at dispatch: `Execute` checks the weapon is drawn and the attack state is none just before it sends the event. `BlowAvailability` in the core sees combat, a weapon, the target, stamina and reach, and cannot see either of those.

A package asks the AI to make the attack instead of pushing an animation past it. The record's IgnoreCombat takes the combat AI's hands away, as it does for a cast, and the procedure (below) retries on every AI update until the graph takes the attack, which a tick-driven event cannot do. That the follower then lands more blows is expected, not measured.

## What the procedure does

Read 2026-09-15 from the running game with `tools/livedisasm.py`; the IDs are the AE ones of 1.6.1170. `BGSProcedureUseWeapon` (vtable 203696) executes through 29381, which goes to the full update, 29383, when the actor's process level is high or middle-high; what it does at middle-low and low was not read. A follower near the player is high.

- **Its inputs.** The engine's parameter table has nineteen: Location, Weapon, Target, AlwaysHit, RepeatFire, VolleyFire, CrouchReload, DoNoDamage, HoldWhenBlocked, VolleyWaitMin, VolleyWaitMax, VolleysPerBurst, VolleyShotsMin, VolleyShotsMax, AllowCombatStart, AimWithoutFiring, AlwaysPowerAttack, DoHeadtracking, BlockPercent. The `UseWeapon` template (01C338) labels them for the Creation Kit: RepeatFire is "Never End?", VolleysPerBurst "End after this many Barrages:", VolleyShotsMin and Max "Min/Max Attacks per Barrage", AlwaysPowerAttack "Always Power Attack?"; it does not expose BlockPercent. None of the nineteen is a bash, and the CK wiki's page names none.
- **A melee blow is drawn from the actor's attack data.** The update walks the attack data map on the actor's base record (`TESActorBase`'s `BGSAttackDataForm`, at +0x130), which carries the race's entries (`docs/ACTIONS.md` 6). It skips an entry flagged Bash Attack or Charge Attack (`flags & 0xA`), one whose chance is 0 (the sprint attacks), and, with AlwaysPowerAttack on, one not flagged Power Attack. With no stamina at all it also skips an entry whose cost is not nothing (26429, which asks a perk entry point); with any stamina, cost is not checked. What is left is weighted by `Actor::GetAttackChance`: the entry's chance, times the combat style's Attack Incapacitated or Power Attack Incapacitated multiplier when the target is incapacitated (47328; the staggered flag is the first thing it tests, the rest not read), or Power Attack Blocking when a power attack meets a blocking target. One is drawn at random.
- **It is performed through the action system, not as a bare event.** A `TESActionData` from the actor, action `ActionRightAttack` (default object 49), or `ActionRightPowerAttack` (70) for a power-attack entry, carrying the entry's event as its animation event, handed to 41557. 41557 sits just before `TESActionData`'s constructor (41558 in CommonLibSSE) and is a thunk through a manager singleton: by that, the perform-action entry mods call `PerformAction`, not confirmed by name. A check comes first (47297, which names "ProjectileNode" and "NPC Root [Root]"; reach or line of attack, not read further). If the action is turned away, the next entry is drawn at once, and the whole draw runs again on the next update.
- **No hand, no bash, no dual action.** The only actions it fetches are `ActionRightAttack` and `ActionRightPowerAttack`, and for a bow or crossbow `ActionRightAttack` to draw and `ActionRightRelease` (51) once drawn. Nothing in the draw looks at what is in the hands: the Dark Elf race (Jenassa's) lists `attackStartLeftHand`, `attackPowerStartInPlaceLeftHand`, the dual-wield, hand-to-hand and mounted entries at chance 1, they pass the filter, and they would go out under a right-hand action. What the graph does with those is not known; turning them away would just move the draw on.
- **It waits for its own swing.** After an action is taken it watches the actor's own attack state and counts the attack when the swing is over. It does not attack while pathing.
- **BlockPercent does nothing visible.** When the target is at Hit or Follow Through and a roll falls under BlockPercent, it calls 37706 with true and holds for 1.2 s. 37706 in this build is `xor al, al; ret`.
- **It ends itself.** When the pause after a barrage has run out, RepeatFire is off and the barrage count has reached VolleysPerBurst, the procedure succeeds (29479 sets done and succeeded), and the UseWeapon node of both templates, `UseWeapon` and `UseWeaponAlreadyHeld`, is flagged Success Completes Package. So Never End off, one barrage, one attack per barrage should end the package after one counted blow: a release signal the UseMagic procedure never gives (`docs/MAGIC.md`, "does not complete on its own"). It also ends without success (29480) on failure paths, one of them after an equip call fails; the others were not identified.
- **It equips.** The update calls `ActorEquipManager::EquipObject` and `UnequipObject` itself, in the part that acquires the Weapon input's kind; when, was not traced. Those calls meet the pins' equip detour (`src/game/Pins.cpp`).
- **Distance.** Its Travel sub-procedure goes to Use Weapon Location, which Bethesda's own combat override sets Near Package Start, radius 500, not at the target. A range test (39519) puts the procedure in a state of its own when it fails; what that state does was not read.

## So, for our blows

- **Power Attack fits.** AlwaysPowerAttack narrows the draw to power attacks; the engine picks which one (in place, forward, sideways, backward) by the race's chances.
- **Bash and Power Bash do not.** The flag test excludes them and no input reaches it. The combat AI bashes through its behaviour tree (`CombatBehaviorBash`, vtable 212595, a thin node over code not traced). A bash sent the way the procedure sends a swing -- the action system, an attack action carrying the `bashStart` entry's event -- is the likely shape of the engine's own, and untried.
- **The action system is also a cheaper experiment for all three.** Sending a blow from the tick as the procedure does, rather than with `NotifyAnimationGraph`, changes the route and not the timing: the combat AI still holds the graph, so it may be refused as often.

## What it would take

- A UseWeapon record per follower beside the cast and shout records, copied from a finished instance of the `UseWeaponAlreadyHeld` template (0F7F4A) the way the cast records copy Mercer's: `TG08BKarliahUseWeaponCombatOverride` (0FCC2A) is one. Put at the front of the follower's package stack under the same `GetIsReference` lease, flag IgnoreCombat as the casts.
- Read every input of the copy: Karliah's has **Always Hit and Do No Damage both on**, Never End on, Target to Attack an alias, Weapon Type "all combat wearable". Ours: Always Hit off, Do No Damage off, Never End off, one barrage of one attack, Target to Attack the rule's target (a single-reference target input, as the cast's is), AlwaysPowerAttack for Power Attack.
- Release on the package completing (the AI dropping it, which the cast lease already watches), with a deadline behind it for a blow never made.

## Open

- **Distance.** Whether the procedure closes on a target beyond reach (the state after 39519), or only swings at one in reach. The core already refuses a blow "too far", so the answer decides only whether that refusal could go.
- **Hands.** What a left-hand, dual-wield, hand-to-hand or mounted event does under `ActionRightAttack`, for a follower with a weapon in each hand.
- **Stamina.** Whether a blow taken this way is charged; the same question is open for the events.
- **Pins.** Whether the procedure's own equip calls fight a pinned hand through the detour, and whether it ever needs them with the weapon already drawn.
- **Low process.** What 29381 does below middle-high.
- **Weapon-bound.** A follower with a spell or a staff in hand has nothing for it to use; the blows already refuse that case ("no weapon").
- **The cheaper change.** Moving "mid-swing" and "weapon drawn" into the snapshot would let the core report a wait instead of firing, and spending the cooldown on the result rather than the decision would let a refused blow retry on the next tick (`docs/ACTIONS.md` 6). Either ends the failure warnings without a package; neither makes a blow land more often.
