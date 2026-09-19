# Tactics for the player

**Status: built in a fight, 2026-09-18, on `wip-player-tactics`; nothing verified in play.** The player is evaluated in a fight as a follower is, under rules of their own on a Tactics tab of their page, with Cast, Power and Shout performed on their own body (`src/game/PlayerCast.cpp`) and the consumables as they were; Attack, the blows, the pins, scrolls and dual casts are left out of their menus. The reading this was built on: the code on master, the CommonLibSSE-NG headers, the load order's Concentrated Poison perk, four Nexus pages, and the attack and shout handlers read from the unpacked 1.6.1170 executable with `tools/disasm.py` ("The handler, read from the executable" below). Every claim about what the engine does in play is unverified until the list under "To verify in play" is run.

The goal as asked: mostly **out of combat** -- keep an armour spell up without recasting it by hand -- and in combat the things that do not take the character away from the player. Out of combat is a second list, built 2026-09-18 on `wip-idle-tactics` and not yet verified in play (below).

## The short answer

- **Potions, food, poisons, soul gems: yes, as they are.** Every one of those actions is an `RE::Actor` call with nothing follower-specific in it (`src/game/Actions.cpp`).
- **Spells: yes, performed on the player's own body, but not the way a follower casts.** A follower's cast is a performance its AI gives, through a package; the player's AI is not running, and giving it the controls takes the character from the player. The player's cast would be the engine's own hand cast, started by a synthesized press of the cast button on a hand lent to it for the cast (Casting, below).
- **Attack, Power Attack, Bash, Power Bash: no.** Attack sets a combat controller's target, and the player aims for themselves; the blows play animations on the player's own body.
- **Out of combat is the larger change, and it is not about the player.** Tactics run only in a fight, in the tick and in the core alike. Buff upkeep needs rules that run outside one, and that change serves followers just as well.

## What carries over as it is

| action | on the player | to check |
|---|---|---|
| Drink, eat (every policy and named form) | `ActorEquipManager::EquipObject`, the same routine the favourites hotkeys go through | whether the queue flag delays it on the player as on an NPC (the NPC note in CLAUDE.md); `playSounds` is false for followers and the player expects the drinking sound |
| Apply poison | writes `ExtraPoison` on the worn copy, no menu | the dose count is hard-coded to 1 (`Actions.cpp`), for followers as much as the player. The engine's count is the menu routine's 1 put through the `ModPoisonDoseCount` entry point; Concentrated Poison (`105F2F`) is that entry point, `Set 2`, three condition tabs. Asking the entry point, as `ChargeWeapon` asks `ModSoulGemRecharge`, lets the perk and any mod's change to it decide. Which of the two item tabs is the weapon and which the poison is to be read before a mod's conditioned perk can be trusted |
| Charge | the engine's own recharge routine, as read for followers | nothing player-specific seen |
| Equip | **built** (2026-09-18, third round): a plain equip through the panel's own call (`WearNow`), no pin; the player's snapshot carries what they wear as their "pins" (`WornAsPins`), so a rule for a thing already worn falls through and "none" takes the kind off, with nothing else changed in the evaluator | in play |
| Cast | **yes, built**: the engine's own hand cast by a synthesized press (Casting, below) | in play, all of it |
| Dual Cast | **yes, built and seen** (2026-09-18): the spell in each hand, the two controls pressed on one frame, which the handler pairs into a dual press (The pairing, below); released at Ready from the left hand's caster. Dragonhide, Spikes and Earth Shield went from both hands in Nordic Souls at the dual price | the engine pairs only behind its own Dual Casting perk check, so the player is offered Dual Cast only where that says yes; the Settings switch waives the perk for followers alone |
| Scroll | **built** (third round): a spell record that is also an item, lent to the hand as the item and cast by the same press; the engine fetches and spends it | in play: the leftover copy stays in a hand that held nothing |
| Power, Shout | **yes, built**: the power or shout selected in the voice slot and the shout control tapped, the same steps as a cast; a shout's first word | the shout handler's press path was read (ID 42430): a press notes the control, a release asks the voice caster; what its gates (IDs 17965, 37901 on the player) refuse is not known |
| Attack | **no** | above |
| Power Attack, Bash, Power Bash | **built** (third round): the engine's own attack actions -- the power attack by the action the handler sends for a hold past its delay (the right, left or dual one by the hands), the bashes by the follower's block-then-attack sequence (`game/Blows.h`), which is those same actions; aimed by nobody, the blow goes where the player looks | in play; the Power Bash perk is asked of the player whatever Settings says, since the idle tree asks it of the player itself |

The snapshot already reads the player correctly where it matters for these: spell cost uses the player's settings (`fMagicCasterPCSkillCost*`, `Sensors.cpp`), perks outside the trees are found for the player, and a spell above skill is not dimmed for the player (`Magic.cpp`). The player's page was built on those readers.

## Casting

**Wanted: the cast played on the player's own body** (decided 2026-09-17), as a follower's is on theirs.

### Why the follower's route cannot be the player's

A follower casts because a UseMagic package at the front of their stack gives their AI a reason to (`dev/MAGIC.md`). The player runs no packages while the player is in control.

| route | verdict |
|---|---|
| **The engine's own hand cast, by a synthesized press**: the spell in a hand, the hands readied, the cast button pressed and released through the attack handler as the player's own input is | **The one to try first.** Everything after the press is the engine's: the animation, the charge, the magicka, the experience, the perks, the sound, the cast event other mods listen for, what breaks invisibility. Nothing to charge by hand and nothing to keep in step with a patch. Auto-Shot and Click-Casting (Nexus 185317) drives the player's casts this way from an SKSE plugin in production: it holds the press virtually and synthesizes the release from in-game events. |
| **An animation, then the effect applied at its release** (`CastSpellImmediate` on the instant caster): what Spell Hotbar 2 does, with animations of its own played through OAR | The hands stay as they are. But no vanilla hands-free cast animation is known (not searched), so it ships animation files, and the mod has been one DLL since 2026-09-08; the cost, the experience and every side effect of a cast become ours to imitate. |
| **The voice**: the spell wrapped as a one-word shout, as a follower's power is (`dev/ACTIONS.md` 7), and the shout key pressed | Vanilla animation, hands untouched; but it is the power's motion, not a spell's, and it borrows the voice slot. |
| **Applied, no animation** (the instant caster alone) | What TactiCast does. The fallback if the hands can never be lent. |
| **Give the AI the controls** (`PlayerCharacter::SetAIDriven`) and push the package | No: the player loses the character for the length of every cast. |

`dev/MAGIC.md` lists `SetCurrentSpellImpl` + `RequestCastImpl` among the routes that failed for followers; those are the caster's internals. The press is a different entry: the input handler, above the caster, which is the path a player's cast always takes.

### The handler, read from the executable (2026-09-18)

`AttackBlockHandler` (`PlayerControls::attackBlockHandler`, a `HeldStateHandler`) was read from the unpacked 1.6.1170 with `tools/disasm.py`. Every ID below is the Anniversary table's number on that build; the SE pair is to be looked up on 1.5.97 when this is built, as `dev/VERSIONS.md` requires. Names in quotes are CommonLib's; names marked *inferred* are read from what the code does, not from a symbol.

**What the handler accepts.** `CanProcess` (vtable slot 1, ID 42454) answers yes only to a `ButtonEvent` whose user-event string is `UserEvents::leftAttack` ("Left Attack/Block") or `rightAttack` ("Right Attack/Block"). The device and the key code are not read. "Dual Attack" is never an input: `ProcessButton` (slot 4, ID 42421) makes it itself (ID 42418) when the second hand's press lands inside a window after the first (a float setting, ID 382104, name not resolved), by rewriting its working copy of the event to `dualAttack`. `UserEvents::forceRelease` releases everything: the four helpers run on a made-up released event and `heldLeft` / `heldRight` clear. That is what a menu sends when it opens (inferred from `PlayerControlsData::setupHeldStatesForRelease` and `HeldStateHandler::triggerReleaseEvent`; the sender was not read).

**What a press does.** `ProcessButton` first honours `ignore`: while set, every event is swallowed until the release of the control named in `controlID`; the dual path sets it on its release, so that the second button's release does not read as a second cast. Otherwise it records the hand's button as held and calls the hand's helper, ID 42438 for the right hand (casting source 1) and ID 42435 for the left (source 0; not read, taken as the mirror). The right helper:

1. If the weapon state is below Drawn (`actorState2.weaponState`) and the event is a press (value not zero, held time zero): `Actor::DrawWeaponMagicHands(true)` (vtable slot 0xA6) and nothing else. **A press while sheathed only draws.** The cast needs its own press once the hands are out, which is why steps 3 and 4 below are separate.
2. Else if the hand has a spell selected (ID 38767: `selectedSpells[1]` is set and its spell type is not Enchantment, so a weapon's enchantment does not count) and `actorState2.wantBlocking` is clear: the cast path, ID 42436, with the source.
3. Else the weapon path (ID 40545 and its neighbours, bow-charge bookkeeping; the swing itself goes through ID 42350 with codes 0x31 and 0x33 on a singleton, ID 400864, not resolved). Not ours.

**The cast path (ID 42436).** The caster is `Actor::GetMagicCaster(source)` (vtable slot 0x5C), with the source forced to 0 when the selected spell answers yes to vtable slot 103 (*inferred*: two-handed) or when a gate on the player's `currentProcess` (ID 38764 calling ID 39382, not resolved) is true. Then:

- **No spell in flight** (`MagicCaster::currentSpell` null): a press, or a hold at or beyond a float setting (ID 376193, name not resolved) with the handler's "press consumed" byte (offset 0x41) clear, calls ID 42417, the **begin**: the caster's source (vtable slot 21), the spell from `selectedSpells[source]`, an item fetched from the player when a player flag is set (ID 17201 over the player's 0x9C4; *inferred*: the scroll or staff in hand), and ID 34401 on the caster with the spell and the item, which is the request that starts the charge (a non-virtual on the caster; CommonLib's name for it not confirmed). The consumed byte is then set. **The charge itself runs on the caster's own update**: nothing in the held branch touches the caster before it is ready, so no stream of held events is needed to charge (to verify in play).
- **State 1 or 2** (CommonLib's guesses: Start, StartCharge): a release calls ID 34408 with true, the **interrupt**: `InterruptCastImpl` (vtable slot 8), three timers reset, `state` = 9, `SpellCast(false)`. **A release before the spell is charged cancels it.**
- **State 3, `kReady`**: a release calls ID 34445: `state` = 4, a counter at 0x44 cleared for a Concentration spell, then `StartCastImpl` (vtable slot 6). **This is the fire.** The caster idles at Ready until the button is let go, which is the held-charged-spell of ordinary play. Then ID 41233 on the player with the caster (not read).
- **State 4, 5 (`kCharging`) or 6 (`kCasting`)**: a press calls ID 40604 and two more (not read; a second press while charging, which does nothing visible in play). A release with a Concentration spell (casting type 2) and the spell's vtable slot 0x2C8 float at or below zero calls ID 34408 with false: **letting go ends a concentration**.

So the sequence the doc asked for holds as read: press once with nothing in flight, watch `MagicCaster::state` for 3, release, and the spell goes; release earlier and it is cancelled. The value asked for in step 5 below is 3.

**The pairing (ID 42418), read in the second round.** `ProcessButton` puts a hand's event through it only when a dual action is possible -- a gate on the player (ID 38765) or melee weapons in both hands. The gate, read in the third round after two presses came out as two single casts: the SAME spell record in both hands, no caster of the four with a spell in flight, the spell's type Spell, and for the player the Can Dual Cast Spell perk entry point answering yes -- the school's Dual Casting perk, or a mod's perk with that entry. Without it the engine never pairs, so the player's Dual Cast is offered only where the entry point says yes, whatever the Settings page asks of followers. Then the FIRST press of either hand is held back: its working copy's user event is blanked (ID 12340 is the engine's empty string), and the press and its control are remembered. What follows decides it: the OTHER hand's press inside a float setting's window (ID 382104) rewrites that second press as "Dual Attack", which the dual helper (ID 42419) begins from the left hand's caster with the dual flag (ID 42417 with r8 set); a hold of the same hand past the window replays it as a plain press of that hand, held time zero; a release inside the window drops it. A dual release is any release once paired: rewritten as "Dual Attack" with the elapsed time, it reaches the cast path on the left caster, which fires at Ready; and `ProcessButton` then sets `ignore` with the other hand's control, so that button's release is swallowed and nothing else is read until it comes. So a dual cast is two presses on one frame and two releases at Ready; and a SINGLE cast with a spell in each hand needs holds after its press, or the press never reaches the caster -- the keyboard's holds do that in play, and `PlayerCast.cpp` sends them until the caster has begun.

**Where the player's own input meets ours.** The handler cannot tell a synthesized event from a real one, and keeps one state machine per hand:

- The player's press of the lent hand's button while ours is in flight lands in the same switch: at state 3 their release fires it, at state 1 or 2 their release cancels it. Either is the player's call and stands.
- A press of the *other* hand's button inside the dual window turns our cast into a dual cast (ID 42418 reads both `heldLeft` and `heldRight`). So ours does not start while the other hand's button is held, read off the handler's own flags, and the release ours sends is the one hand's only.
- The key physically held after our synthesized release: the device keeps sending held events, the consumed byte is clear again, and a hold at the threshold begins a new cast of whatever is in the hand. To watch for in play; the answer may be to give the hand back only after the player's button is up.
- A menu opening mid-cast sends ForceRelease, which fires or cancels ours as it would the player's. The watcher sees the state leave and abandons, as it would on any missed window.
- **Call the handler directly**, `attackBlockHandler->ProcessButton(event, &controls->data)`, on the game thread, rather than queueing through `BSInputEventQueue::AddButtonEvent`. The queue is the whole input pipeline: capped at ten button events a frame, delivered to every handler and every other plugin's input sink, which would see a phantom press. The direct call reaches this handler alone. What it skips is the gating `PlayerControls` does before its handlers, `ControlMap::IsFightingControlsEnabled`, `blockPlayerInput` and menu mode (the sink itself was not read), so those are checked first by hand, alongside the gates under Out of combat.

### A cast on a lent hand

The steps, each a state read on the fast tick, as a bash's are (`game/Blows.h`):

1. **Choose the hand**: one already holding the spell; else one empty or holding a spell; else the left, unless the spell is two-handed. A two-hander or a bow means both.
2. **Lend it**: remember what the hand holds -- which copy (`dev/UNIQUE.md`) -- and equip the spell there. The player's equip is immediate, unlike an NPC's queued one (unverified).
3. **Ready the hands** if they are sheathed (`Actor::DrawWeaponMagicHands`); wait for drawn.
4. **Press**: one `ButtonEvent` for that hand's attack control, value 1 and held time 0, to `AttackBlockHandler::ProcessButton`, with no cast in flight on that hand and the other hand's button up. The caster charges on its own update; no held events follow (to verify).
5. **Release** when that hand's caster reaches `MagicCaster::State::kReady` (3): a `ButtonEvent` with value 0 and a held time above 0. Then wait for that hand's spell-fire event naming the spell, the signal a follower's lease ends on.
6. **Give the hand back**, and sheathe if they were sheathed.

What that costs the player, to be seen in play: a sheathed character draws and sheathes for every upkeep cast (in town, a guard's line about it); what was in the hand is out of it for a second or two; and the player's own press of the same button during the cast meets ours. A step that does not come within its window abandons the cast and gives the hand back.

When not to start one, beyond the gates under Out of combat: mid-swing, blocking, a bow drawn, a spell of the player's own charging in either hand, sprinting.

## Out of combat

**Decided 2026-09-18: a second list, not a field on the rule.** The per-rule In combat / Out of combat / Always recommended below was built and taken out the same day: a rule's moment is not a property of the rule but of the list it sits in. Out-of-combat tactics are to be a list of their own beside the combat one -- a tab, or a section that folds -- the same editor over it, evaluated out of a fight as the combat list is in one. The rest of this section is what that needs of the tick and the gates, and stands.

### As built (2026-09-18, `wip-idle-tactics`; not yet verified in play)

**A list carries its moment.** `RuleSet::moment` (`core/Rule.h`) is Combat or Idle, and `Evaluate` reads it: the combat list decides in a fight and on its two edges as before; the idle list decides out of a fight and nothing in one, is never handed an edge, and its standing rules run on exactly the ticks the combat list's do not. Each actor has one list of each, in the tick's map per moment (`GetRules(id, moment)`; a set knows its moment, so `SetRules` takes no second word), in the save as `idleRules` beside `rules` (`dev/PROFILES.md`), and on the panel as an **Idle Tactics** tab after Tactics, on a follower's page and the player's, the same editor over it (`DrawTacticsTabs`).

**One context serves both lists.** A cooldown is the action's, not the list's: a potion drunk on the fight's last tick is not drunk again on the first tick after it. A list in progress is whichever began last, and the tick remembers which (`FollowerState::moment`): the Combat end lists run on after the fight before the idle list gets a turn, and the fight's first edge drops an idle list in progress as it drops anything else. Tested in `tests/test_idle.cpp`.

**Which list a tick evaluates** is `ListToEvaluate` (`Tactics.cpp`): the combat list in a fight, on its farewell, and while a list of its own is in progress; else the idle list, while it has rules or a list in progress; else nobody, so an actor with no idle rules costs no snapshot out of a fight, as before. The same for the player, under the holds `PlayerHeld` already gates. The events log's `rule.fired` carries `list`, "combat" or "idle".

**What the idle list leaves out** (`IsSubjectValidIn` and the four beside it, `core/Rule.h`): there is no enemy out of a fight, so no Enemy subject and no Attacking or Attacked by with it, no Enemy or Attacker target, no Attack and no blow; the edges are the combat list's; Hit by is a fight being had; Fleeing is a fight's state. Everything else -- the measures, the other statuses, what is wielded, the summons, the corpses, the allies' extremes -- is offered and answered as in the combat list. The editor leaves them out of the idle list's menus; a hand-edited profile carrying one reads InvalidCondition, as a pair the subject cannot answer does. One status came with it, about anyone: **Diseased**, an effect running whose spell is of the Disease type. (Injured, health under its maximum, was built and taken out the same day: the health percent conditions say it.)

**Followers too.** The tab is on every page; a follower's cast through the package has never been run outside a fight, and that is the first thing to watch.

### The gate before it

Tactics were a combat system by construction, in three places:

- the tick evaluates an actor only while fighting, on the farewell evaluation, or with a list in progress (`Tactics.cpp`, `Tick`);
- `Evaluate` decides nothing out of a fight after the Combat end lists (`Evaluator.cpp`, "Out of a fight only the Combat end lists run");
- `PredicateKind`'s comment says there is no "in combat" condition because tactics only run in one.

### What it needs

**Upkeep itself is already there.** `EffectAlreadyActive` makes a cast unavailable while its spell's effect runs, so `IF Self: Any THEN Cast Oakflesh` is a maintained buff the moment it is evaluated out of a fight: it fires when the effect is gone, reports "that spell is still running" while it is not, and the next rule gets the turn. The gap between expiry and the recast is one turn, at most half a second. Refreshing before expiry is not wanted yet.

**What a rule needs is to say when it runs** -- and the answer is the list it is in (decided above). A rule wanted in both lists is written twice, which is the cost accepted; an "Out of combat" predicate would take the rule's one condition, so `Out of combat and sneaking -> Muffle` could not be written, and a field on the rule puts a second axis on every row. The existing edges keep their meaning: Combat begins and Combat ends are the combat list's, and their moment is the edge.

**Cost.** Out of combat the tick builds a snapshot for an actor with at least one idle rule, and for nobody else. Measured 2026-09-18 in Nordic Souls: **20 ms per evaluation** of the player, every half-second, three idle rules. Reading the bag on demand (`dev/PLAN.md` 3.2's dependency-driven activation) was built and taken out the next day: with it the evaluation read 19 ms and no bag walked, so the four walks of the bag were about a millisecond of the twenty. The one evaluation before the two followers appeared cost under a millisecond. So the cost line times each step of the snapshot -- self, party, each other actor's traits, hands, spells, effects, the bag -- and the log named it: **spells, 19 ms of the 20** (self 80 us, party 50 us, the bag 800 us). That step priced every spell the player knows through the engine's `CalculateMagickaCost`, twice for the dual cast, which walks the perk entry points per spell; only a Cast rule's own spell is ever read from the prices. Since 2026-09-19 `BuildSnapshot` takes the spells the actor's rules name (`SpellsNamedBy`, both lists) and prices those alone; every spell is still listed as known and in the loadout. The corpses were never walked (level and distance only), nor are the allies' and enemies' bags (the armour figure is two engine reads).

**When not to act, though time runs.** The clock gate already holds everything in a paused menu or behind the panel. Out of combat the player is also in states where an automatic cast or drink is wrong: in dialogue (the dialogue menu does not pause), with controls disabled by a scene (`ControlMap::IsFightingControlsEnabled`), in furniture or at a crafting station, mounted (`IsOnMount`), in a kill move, in beast form, invisible (a cast breaks it, as the player's own would, so a buff rule must not undo the player's Invisibility), and with the 3D not loaded. Each a state read on the tick, not a timer.

## The player in the rule model (as built)

- **Self is the player.** The Player subject and target are the same person, so neither is offered in the player's list (`view.player` in `UI.cpp`); Ally is the followers, a named Follower is one of them.
- **The snapshot's ally list** puts the player first for a follower; built for the player it leaves them out (`BuildSnapshot`, `Sensors.cpp`), so the party is the followers.
- **What the player cannot do** is `PlayerSupports` (`PlayerCast.h`): the tick marks those actions off in `Capabilities::unsupported`, and the menus leave them out. No dual cast: the snapshot marks nothing dualable for the player.
- **The tick** finds the player by hand after the followers (`Tactics.cpp`), with the same state a follower has, and skips what is follower-only: no cast records, no pin pass, no packages. Held, and said once each way as `player.held` / `player.free`, by `PlayerHeld`.
- **The save**: `IdentifyFollower` on the player gives the base record `Skyrim.esm-7`, which is stable, and the co-save is per save, so two characters do not meet. The profile's `followerName` field holds the player's name.
- **The panel**: the player's page is a follower's view with `player` set; Tactics is its last tab, Combat Style is not offered, and a Combat Style carried from a follower's page opens Tactics (`CarriedTab`). Its build (`FillTactics`) scans the whole bag and spell list, on a change of page, never on a beat.

## Prior art

- **TactiCast** (Nexus 180626, v2.5, 2026-09-07): conditions set in SKSE Menu Framework cast known self spells on the player, non-targeted and non-concentration only, magicka spent, spells skipped when maximum magicka is too low. Leaves out powers and shouts. Notes that a recast of a bound weapon dispels it and that later casts break invisibility. Its source is in its optional files: the reference for the applied fallback.
- **NPCs use Potions** (Nexus 67489): the potions, poisons, fortify potions and food it drives for NPCs are "optionally also supported for the player character". Our drink call is copied from its NPC path; its player path was not read, but a production mod drives the player's potions from an SKSE plugin.
- **Auto-Shot and Click-Casting** (Nexus 185317, v2.0, 2026-08-10): the player clicks once and the plugin holds the cast button virtually and synthesizes the release when the charge completes, from in-game events rather than timers; a menu opened mid-cast cancels the cast as a real release would. Has a guard for a "dead hand", a cast stuck mid-way, which it returns to idle after 1.5 s. The press route above, in production, for hand casts, scrolls and staves.
- **Spell Hotbar 2**: casts from a hotbar with any weapon in hand, the animation played through DAR/OAR; the animation-then-apply route.

## To verify in play

One session, a character with Oakflesh, vanilla Healing, a racial power and a shout, some potions, `FollowerTactics.log` at debug, a rule `IF Self: Any THEN Self: Cast Oakflesh` on the player's Tactics tab, and a fight:

1. The cast: `rule.resolved` from module `player` -- `highestState` 3 says the press charged the caster, `firedS` that the release fired it, `lent` and `drew` what was done to the hands, and the hand back after. Then the same from sheathed (the reading says the first press only draws), with a sword in the lent hand (does the sword come back to the same hand, with its copy), and with the real key still held when the release is sent (the reading says a hold at the threshold begins another cast). **Seen 2026-09-18 (Flames, a stream): pressed 0.2 to 0.4 s after the request, streaming 0.5 s after the press, released after the 3 s sustain, magicka drawn as the game charges it, an Iron Dagger put back to the lent hand. A hand that held nothing keeps the spell: unequipping it made the next lend play the equip animation twice.**
2. Healing, a concentration spell: does the stream start on the press and end on the release after the 3 s default. **Seen with Flames, above.**
3. The power by the shout control, and the shout: `voice fired`, the previous selection put back. **Seen 2026-09-18 in Nordic Souls: Dragon Aspect held 0.92 s until the process's charged variation read 2 of 2, released, the voice fired 0.12 s later, all three words; Shadow Form, Blood Mist and Aspect of the Rain went off on the release with no voice animation and no fire event, the engine's used-power list taking each at once, which is a power's fire signal now. The actor's shout level is not the words charged: it read 2 of 2 thirty milliseconds after the press.**
4. The player's equip of a spell into a hand: at once, as `Lend` assumes with its one-second window, or queued to the next update as an NPC's is. **At once: pressed 0.02 s after the request with the spell already in hand, 0.2 to 0.4 s after a lend.**
5. The player's own press on the lent hand's button mid-cast, and a menu opened mid-cast: which reason the run ends with.
6. `EquipObject` on a potion for the player, queue flag set and not: when health moves, whether the drinking sound plays.
7. `BuildSnapshot` on the player: its cost, in the tick's cost line, with a real bag.

## Open

1. Lending a hand: the draw, the sheathe and the swap back for every upkeep cast. Acceptable, or should a cast wait until a hand is free or the hands are already drawn?
2. The idle list in play: a follower's cast through the package out of a fight, the player's buff upkeep (`IF Self: Any THEN Cast Oakflesh` on the Idle Tactics tab, recast when the effect is gone), the switch from one list to the other across a fight's edges, and the snapshot's cost on the player's bag every half-second out of a fight.
