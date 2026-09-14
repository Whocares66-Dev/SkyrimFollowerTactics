# What is still to do

Kept short and current. Each item says what is missing and, where known, what
it takes. Measured facts behind them are in `docs/MAGIC.md`; the phase plan is
in `docs/PLAN.md`.

## Casting

- **Deferring to the AI's own cast, in play.** A cast rule now waits
  (verdict "casting") while the follower is mid-cast on a spell of their
  own, instead of interrupting it (2026-09-04, Lightning Bolt on "magicka
  above half" cut off every spell Marcurio began). The risk is the other
  way: an AI that casts back to back never lets the rule through. Watch
  for a cast rule whose Status sits on "casting" for whole fights; if it
  does, the next knob is the cast cooldown, 2 s to 4 s, or letting the
  rule through once it has waited some seconds.
- **Sustain length in the editor.** A concentration rule's stream length is
  the rule's numeric argument, which the panel does not expose; every stream
  runs the 3 s default. Likely shape: a random length within a range, which
  the record supports natively (`CastTimeMin` / `CastTimeMax`).
- **The AI sometimes never starts the cast.** Package selected, rank set,
  `CastStop` on her own attack, then nothing until the 2.5 s deadline. About
  one request in four in the Flames runs. Unexplained; candidates are the
  procedure's own cooldown inputs (1 s) and the target moving out of range.
- **Non-hostile targeted spells.** Healing Hands is delivery Aimed and would
  be cast at the enemy. Needs: a hostile check on the spell's effects, and the
  player as the target when it is not hostile. Not authored yet, so untested.
- **A target picker in the editor.** The rule model has one
  (`ActionTargetKind`); the panel does not show it, and casts ignore it in
  favour of the spell's delivery. Fine until someone wants "heal the player"
  and "heal self" as separate rules for one spell.
- **Concentration spells on self.** Vanilla Healing is concentration with
  Self delivery. The sustain path should apply unchanged; not yet run.
- **Drinking while mid-cast.** Allowed by design (a potion needs no hands).
  Whether equipping a potion interrupts an NPC's cast in progress is not
  verified; one log hint says it might. If so: treat "holding a record" as
  busy for potions too.
- **Shout, in play.** Built 2026-09-05 on the shout pool with the shout itself in the package's input (`docs/ACTIONS.md` 7); not yet run. `bat ftmake` teaches Unrelenting Force; the rule is `IF target: any THEN target: Shout -> Unrelenting Force`. The voice recovery is read (`Actor::GetVoiceRecoveryTime`, per actor, NPCs included) and a shout inside it reports "recovering" instead of firing; not yet seen counting down in play. Open: whether a three-word shout's long animation wants the begin step-back's 3 s or more.
- **A power's Voice type for the lease.** The power's shared record reads Type Voice for the ~2 s of a lease, restored on release (`docs/ACTIONS.md` 7). Works; the clean alternative is Voice spells of our own in the ESP carrying the power's effects, which needs the active-effect check taught which spell stood for which power. Only worth doing if the window ever shows.
- **Voice pins, in play.** Built 2026-09-05: the Magic tab's Equipped cell for a power or shout readies it in the voice slot, pins it, or puts it away, like a hand cell; one voice pin sets every other power and shout aside with the "<x> is pinned" tooltip; the watchdog puts a pinned one back. Not yet run. Not covered: the combat AI's own shout entries are not in the score hook (only weapon and spell entry classes are), so an AI that shouts its own shout mid-fight is put back by the watchdog a tick later rather than kept from it.
- **Make-room unequips removed, in play.** 2026-09-05: when a pin displaces another, only the book changes and the engine's equip does the taking-off; the explicit unequip that followed was a leftover from the prevent-removal flag and, for the voice, undid the new equip. To watch for once each: weapon over weapon in one hand, spell over spell, armour over armour, a spell into a hand holding a weapon, a two-hander over sword and shield. If any leaves the old thing on, that case gets its unequip back.
- **Night Eye on the player.** Vanilla: `magicNightEyeScript` applies its image-space modifiers whoever the effect's target is, so a follower's Embrace of Shadows or Night Eye tints the player's screen. Not ours to fix unless it grates; the fix would be a script override that skips the modifiers when Target is not the player.
- **Summon and Corpse, in play.** Built 2026-09-05 (`docs/CONDITIONS.md` 6a); not yet run. `bat ftmake` grants Conjure Flame Atronach and Reanimate Corpse; kill a bandit from `bat ftman` for a corpse the spell will take. Rules: `Self: Summon none -> Self: Cast Conjure Flame Atronach`, and `Corpse: Highest level -> Corpse: Cast Reanimate Corpse`. Open: whether the UseMagic package aims at a dead actor; whether a raised corpse lands in the follower's `commandedActors` (the Summons tab and Summon: Active depend on it); and the 3000-unit corpse reach.
- **The follower's own spell numbers, in play.** 2026-09-05: the Magic tab's magnitude, duration, effect lines and description, and the Reanimate cap, are the follower's -- the record's number through the ModSpellMagnitude / ModSpellDuration perk entry points, which is how the engine makes the effect. Cost was already theirs. On a vanilla follower this changes nothing from the record for Fortify gear, and rightly: the two hidden perks that turn the Fortify values into anything (`PerkSkillBoosts` for enchantments -> cost, `AlchemySkillBoosts` for potions -> magnitude or duration, `docs/RESEARCH.md` 6) are on the player and the race presets only, so Fortify Destruction on a follower's robe does nothing in game and the panel shows nothing. Real perks do apply: a follower given Augmented Flames should show Firebolt above the record's 25, and the description should agree with the effect line. A mod that distributes the two perks to NPCs is picked up without a change here. The description is asked for with no parent so its tokens are left for us to fill; if it comes back already filled (the engine's, for the player), the parent-less call is not what it looked like.
- **Food and ingredients, in play.** Same build. Whether an NPC gets the effect of a food or an ingredient it eats through `EquipObject`, and what the settle time should be; the potion's 3 s is used meanwhile.

## Rule engine

- **Equip rules, in play.** The four equip actions pin through the same
  book as the Inventory tab, and the evaluator is tested; what is not yet
  verified in game is a pin landing mid-fight when she holds something
  else -- whether the AI switches to it, or is only kept from switching
  away once she has it -- and what the game does after "Equip armor: None"
  takes pinned pieces off (her outfit may not come back until a cell
  change).
- **A hand pin back after a cast, in play.** The watchdog now puts a
  pinned weapon or spell back mid-fight as soon as our cast has released
  its hand (`PutBackNow`, after Marcurio was left dagger-less from his
  first Lightning Bolt to the end of the fight). Watch the log for the
  put-back landing after the spell equip of the NEXT cast -- an
  "InterruptCast" right after "putting it back on" -- in which case the
  in-fight put-back should equip immediately rather than queued. Also
  whether the combat AI re-equips its own spell into the hand once the
  dagger is back (it should not: the score hook zeroes those entries).
- **Pins over a fight, in play.** The book is remembered on entering
  combat and restored on leaving it (`NoteFight` / `RestorePinsAfterFight`
  in Pins.cpp). Verify: a travelling outfit and dagger come back after a
  fight in which rules pinned a bow and Flames; a panel pin made mid-fight
  survives; a fight that flickers off and on (IsInCombat dropping for a
  tick) restores and re-remembers without a visible swap.
- **The equip detour, in play.** `RefuseEquipsAgainstPins` refuses the
  engine's own `EquipObject` calls that would break a pin (the sword it
  puts in the right hand when a fight ends, over pinned Flames). Verify
  the log shows the refusal and Jenassa keeps Flames. The watchdog's
  two-try stand-down for spells is gone: a repeating "readying it again"
  line now means the detour missed a path, and prints the hand state. `EquipSpell` (37939 / 38895) could be detoured the same
  way to hold a pinned hand against the AI's spell choice, but a CastSpell
  rule's package equips through it too and must be exempted (the lease
  knows the spell), or tactics stop being unlimited.

- **The new conditions, in play** (docs/CONDITIONS.md 8): which of
  `kParalyzed` and the archetype flips first; how long `staggered` holds;
  whether the hit event fires for cloaks, hazards and concentration ticks;
  logged armour figures for a fight's enemies against the estimated tiers.
- **Attack, in play** (docs/ACTIONS.md 6). Whether the standard target
  selector lets a written `targetHandle` stand: `Ally: Attacked by Ranged ->
  Attacker: Attack` against a bandit archer, and read the log for
  "already fighting them" on the next tick, or the rule re-firing every
  two seconds. If it snaps back, the fallbacks are a selector vtable hook
  or a UseWeapon pool. Also whether a hit event's `projectile` is set for
  every arrow and bolt, and never for a thrown or melee hit.
- **Cast on a chosen target, in play.** A targeted spell now goes at whom
  the rule aimed it (the Then cascade's first level: self, the ally or
  enemy the condition matched, the player, a named follower, the target,
  the attacker). Verify Heal
  Other lands on the hurt ally and on the player through the package's
  Target input, and what an aimed stream (Healing Hands) does at a moving
  ally.
- **Group subjects, in play.** Allies and enemies are sensed by
  definition (docs/CONDITIONS.md 6): the player and the teammates, and
  whoever is in combat and hostile to the player. Verify against the log's
  once-per-fight "allies / enemies" line that a dead enemy drops out and a
  distant hostile is not counted before the player is in its fight.
- **Tactics in the save, in play.** Written by the SKSE save callback
  and taken back at first sight after a load (`docs/PROFILES.md`), built
  2026-09-04 and not yet run in game. Verify: a rule survives save and
  load ("saved N follower record(s)" and "N rule(s) ... from the save" in
  the log); an earlier save shows its own, earlier rules; a quicksave
  carries them too; a dismissed follower's record survives a save made
  while they are away ("carried from the loaded save"); a pin survives,
  and a pin on a thing sold before the save is forgotten ("does not hold
  -- not worn now" in the log).
- **Pins without the prevent-removal flag: watch the log.** The flag
  outlived the mod (2026-09-04: DLL removed, an elven sword handed
  over, the pinned iron dagger's unequip refused and the sword's equip
  done anyway, both marked equipped in one hand), so pins no longer set
  it and rely on the equip detour, the score hook and the watchdog
  alone. Played the same day and found fine. What the flag used to
  cover -- the best-weapon swap on leaving a fight, the outfit refresh
  on a cell change, handed-over armour -- is the detour's alone now: if
  the watchdog's "took off pinned ... putting it back on" line repeats
  out of combat, the detour has missed a path the flag was covering.
- **Shareable named profiles.** One file per follower is the whole of
  it today; copying a list between followers, or a profile a forum can
  hand round, is a second kind of file over the same format.
- **Named-potion effect check.** `DrinkPotion` has no "already in effect"
  test; only its per-potion cooldown spaces it. The strongest-of-a-kind
  actions do check the restore effect.

## Panel

- **The player's page, in play.** Built 2026-09-13 on `wip-player-stats`: Follower Tactics > Player, the sheet's six tabs over the player, read when the panel opens and after each click, the equip cells equipping and unequipping only (no pin, no ban). Not yet run. The page is the check `docs/MODIFIERS.md` asks for: its Damage, Armor, Cost and Magnitude figures should equal the game's own inventory and magic menus, and an Other line on a hover is a formula that is wrong for the player (damage and spell cost branch to the player's settings; the armour curve is the engine's own). To watch: the Skills tab lists the tree perks taken, and Other Perks a stone's or a quest's perk (found through the load order for the player, not the base record); the Shouts chip lists the shouts learned (read off the base record's spell list, where the player's are expected to be; unverified); the Health, Magicka, Stamina and Carry Weight hovers, where a level-up's increase may show as "Perks and race", a label written for NPCs; the log's "player page built in N ms" with a large bag. With `FreezeTimeOnMenu = false` the page stays as it was at the open.

- **Help as a collapsible section, not per-row tooltips.** A greyed row's
  tooltip gives the reason only ("Firebolt is pinned";
  "Needs: Destruction (50) / Has: Destruction (39)"). What a pin promises,
  what "set aside" means for the AI and for cast rules, and that a spell
  above skill can be cast by a rule but not pinned, belong in one place the
  reader opens on purpose.
- **An item's model on its detail page.** ImGui draws textured quads, not
  meshes, so a 3D preview means one of: (a) the game's own
  `Inventory3DManager` (`UpdateItem3D` / `Clear3D`), which renders the
  selected item into the frame the way the vanilla inventory does -- cheap,
  but it draws behind the framework's overlay and is positioned for
  Scaleform's layout, so whether it can be made to sit inside our window is
  unverified; or (b) rendering the NIF ourselves into a D3D11 render target
  and handing the shader-resource view to `ImGui::Image` -- always works,
  but is a mesh loader and a renderer of our own. The framework's
  `LoadTexture` takes a file path only, so a 2D icon per item type is the
  one cheap option available today.
- **Enchantments show late while the clock is frozen.** With the framework's
  `FreezeTimeOnMenu = true`, equipping an enchanted piece from the Inventory
  tab changes the Equipped column and her model at once (the click path
  equips unqueued and refreshes the model), but the enchantment's effect --
  robes of Destruction's -17% cost on the Skills tab -- lands only on her
  next update, after the panel closes. Do NOT pre-apply it with
  `Actor::UpdateArmorAbility`: the engine's own application still follows
  when time runs, and the two stacked to -34% (2026-09-02). Either accept
  the lag or run the panel with `FreezeTimeOnMenu = false`, where the tick
  refreshes the sheet within half a second.
- **A dismissed follower's entry stays.** Removing a menu entry needs the
  framework's `DeleteSection`, committed to its repository on 2026-09-02 but
  in no published build (3.14.1 exports only `AddSectionItem`). The code
  already calls it and frees the slot when it succeeds; until a release
  ships it, the entry says "Dismissed". The call needs the SDK header from
  that repository (`resources/SKSEMenuFramework.h`, gitignored here at
  `extern/SKSEMenuFramework/`); the 3.11 header on Nexus predates it.

## Harness and diagnostics

- **Structured log — built 2026-09-09, not yet verified in play.** `docs/LOGGING.md` is what it does. What is left is a session with the game up: that the ini is found under MO2's virtual file system, that `.events.jsonl` lands beside the log, and that a fight's lines read the way the level table says they should at `info` and at `debug`.
- **Recruiting from the console.** `cqf DialogueFollower SetFollower <refid>`
  should fill the alias without the dialogue; unverified.
- **Pool tests.** The package pool and lease have no unit tests because they
  touch the game. A seam that lets the tick and release logic run against a
  fake actor would cover the release paths.
- **What the core tests never reach** (coverage, 2026-09-09; `.\tools\build.ps1 -Preset core-cov -Coverage`, line-by-line in `build\core-cov\coverage\html`). 98% of `src/core` lines; `Profile.cpp` is complete. What is left is by construction: the resolver's branch for an enemy's attacker (the validity matrix never lets that pair through), the `default:` arms of exhaustive switches, and the `Verdict` words' final `"?"`. Nothing worth a test.

## Toolchain

- **CommonLibSSE-NG migration** to alandtse `ng` (`docs/COMMONLIB.md`): brings
  `ForceRefTo`, and re-check the `Skyrim.INI` log-directory quirk.

## Left from the 2026-09-11 code review

The review's bugs, dead code, duplication and wording items are done (the commits of 2026-09-11). What it also asked for, and is not done, each with why:

- **Verify in play what the review could not.** The cast sink's atomics, the score hook under its lock, the equip detour's one-copy rule, the per-hand poison and charge readings, and calibration refusing an inline canary all compile and lint clean and have not been run in a fight.
- **Split `UI.cpp` (rule editor, sheet pages, widgets) and `Sensors.cpp` (snapshot, sheets), and move the spell math out of `Inventory.h`.** Mechanical but large, and only verifiable in play; the shared pieces both halves would need are in `game/Sheet.h` now, so the split is a move.
- **One `PanelState` per follower** in place of the five per-follower maps and three filter buffers, and `InventoryTabState::select` written by three tabs.
- **The engine's own name tables** for the condition functions and the perk entry points (`SCRIPT_FUNCTION::GetFirstScriptCommand`, `BGSEntryPoint::GetEntryPoint`) in place of `ConditionNames.inc` and `kEntryPointNames`. Needs a play session to confirm the engine's strings match the Creation Kit's wording.
- **Smaller repeats left in `src/game`:** the cast and shout request prologue in `Packages.cpp` (a `ResolveTarget` and a `ClaimSlot`; the `skyrim_cast` -> `FindInputUID` -> `InputByUID` chain six times), the spells-shouts-scrolls enumeration in the snapshot against the panel's scans, the Fortify-factor block three times, the Speed row's own `ValueNote` threshold, `%08X` inline against `HexId`, the two `ReplaceNoCase` copies, the perk-description read four times, the panel's filter-sort-tiebreak three times and its widget repeats (chip rows, detail headers, up-down-delete, table-in-pieces, stat rows). Each is small; none has cost anything yet.
- **`/we4062` with no `default:` on switches over the rule enums,** so a new `ActionKind` cannot be half-added. Every classifier is one function now, which covers the case the review found; the compiler check would cover the next one.
- **`Profiles.cpp` and `Tactics.cpp` include each other;** the profile assembly in `LoadIfNew` and `ProfilesToSave` would move into `Profiles.cpp` with the identities.
- **`Downgrade-Skyrim.ps1 -Step check` and `check_install.py` check the same things twice.**
