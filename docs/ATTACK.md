# Blows through a UseWeapon package

**Status: an idea, not built (2026-09-14).** Power Attack, Bash and Power Bash are sent today as one animation event each, with `NotifyAnimationGraph` (`src/game/Actions.cpp`); their design and first measurements are `docs/ACTIONS.md` section 6. This note is the case for trying Bethesda's own route instead: a UseWeapon package on the follower's stack for the length of a lease, the way casts already go through UseMagic (`docs/MAGIC.md`).

## Why

The follower's combat AI drives the same animation graph, so an event sent from our tick usually arrives while a swing, a block or a recovery is in progress, and is turned away. Measured 2026-09-09: 3 of 11 blows landed. Seen again 2026-09-14 in Nordic Souls: a Bash rule on Ghorbash failed seven times in 34 s, five "already mid-swing" and two "the weapon is not drawn", each a `rule.actionFailed` warning.

Each of those also spent the action's cooldown. The core stamps the cooldown when it decides, and the refusal is found only at dispatch: `Execute` checks the weapon is drawn and the attack state is none just before it sends the event. `BlowAvailability` in the core sees combat, a weapon, the target, stamina and reach, and cannot see either of those.

A package asks the AI to make the attack instead of pushing an animation past it. The AI should then fit the blow between its own swings and draw the weapon itself, which are two of the three reasons a blow is refused today; the third is the graph refusing a blocking, staggered or recovering actor. That is expected, not verified.

## What it would take

- A UseWeapon record per follower beside the cast and shout records, copied from a finished instance of the `UseWeaponAlreadyHeld` template the way the cast records copy Mercer's (`TG08B*UseWeaponCombatOverride` is Bethesda's; its inputs are listed in `docs/ACTIONS.md` 6), put at the front of the follower's package stack under the same `GetIsReference` lease.
- Its inputs set per request: Target to Attack to the rule's target, Always Power Attack for Power Attack, and a barrage count and Max Time spent Attacking short enough that one blow ends it.
- A release signal: an attack animation event of the follower's own, as the spell-fire event releases a cast, with a deadline behind it. Which event, and whether it carries enough to tell our blow from the AI's, is to be found.

## Open

- **Bash and Power Bash.** The template's recorded inputs have no bash option. Whether the procedure can be made to bash, or those two stay animation events, is unknown.
- **Timing.** Whether the package waits for a swing in progress to finish or cuts in, whether it honours the combat style's rhythm, and whether it closes the distance to a target beyond reach.
- **Weapon-bound.** A follower with a spell or a staff in hand has nothing for it to use; the blows already refuse that case ("no weapon").
- **The cheaper change.** Moving "mid-swing" and "weapon drawn" into the snapshot would let the core report a wait instead of firing, and spending the cooldown on the result rather than the decision would let a refused blow retry on the next tick (`docs/ACTIONS.md` 6). Either ends the failure warnings without a package; neither makes a blow land more often.
