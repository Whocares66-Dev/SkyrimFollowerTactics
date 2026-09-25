# What is still to do

Kept short and current. Each item says what is missing and, where known, what
it takes. Measured facts behind them are in `dev/MAGIC.md`; the phase plan is
in `dev/PLAN.md`.

## The player

- **The player's cast, power and shout: seen in play 2026-09-18** (`dev/PLAYER.md` "To verify in play"): a stream (Flames), fire-and-forget spells single and dual (Dragonhide, Spikes, Earth Shield), a three-word shout (Dragon Aspect) and three powers, a dagger lent and put back, a menu opened mid-cast. Still to see: a cast from sheathed hands (`drew` true), the player's own press on the lent hand's button mid-cast, a follower's cast rule and the player's on the same tick over many fights, and the day turning on the used-power list.
- **The idle list in play** (built 2026-09-18 on `wip-idle-tactics`, `dev/PLAYER.md` "As built"): a follower's cast through the package out of a fight, the player's buff upkeep, the hand-over between the lists across a fight's edges, and the cost line after the spells are priced by name (`dev/PLAYER.md` "Cost"): the player's evaluation should read about a millisecond, the spells step under a millisecond with a few named. Diseased in play too: whether a disease shows as an active effect of the Disease spell type on this build.
- **A single cast with a spell already in the other hand.** The handler's pairing holds such a press back until the holds sent after it outlast its window (`dev/PLAYER.md` "The pairing"); the holds are sent, and the case has not been seen in play: `pressedS` to `readyS` should grow by about that window.
- **A greater power's once a day, on the player.** Seen 2026-09-18 (dev): Battle Cry by the shout control fired as often as the rule liked. The engine marks a used power in the caster's SpellCast callback (`Actor::AddCastPower`, read from 1.6.1170: ID 34144 adds a power, lesser power or a shout's words) and its cast check refuses one on the list (ID 34145). In Nordic Souls the list took every power at the release itself, greater and lesser, and a lesser one was off it again by the next request; `PlayerCast.cpp` adds a greater power that fired only when the list has not. The snapshot reads the list for every actor, so a used greater power's rule falls through. To watch: a vanilla greater power on this build (Battle Cry: does the list take it, is the second firing refused), and the next game day resetting the list.
- **The lend and put-back per cast.** A rule that casts every cooldown from a hand that does not hold the spell lends it every time, draws if sheathed, and puts back what the hand held. Settled 2026-09-18: a hand that held nothing keeps the spell, since unequipping it made the next lend into that hand play the equip animation twice (the engine's, on one equip call), and with the spell left in place it plays once. The way out of the churn for a combat spell is an equip rule for the player (a plain equip, no pin), not offered yet.
- **The player's equips and scrolls, in play.** Built 2026-09-18 (third round). The blows are seen: a power attack by the engine's action for the hands, a bash and a power bash by the follower's block-then-attack sequence, each charging stamina as the player's own do. Not yet seen: a plain equip and its "none" through the panel's own call, and a scroll lent as the item and cast by the press. To see: `rule.fired` `performed` for an equip, an equip rule reporting "already pinned" on the next tick, which for the player means worn, and a scroll's `rule.resolved`.
- **The hand's button still held after our release.** A hold past a threshold begins another cast (read from the handler); if it shows, the hand goes back only once the button is up.

## Casting

- **A follower's cast paying for itself** (built 2026-09-25 on `wip-cast-cost`, issue #2, `dev/MAGIC.md` "What a cast costs"). Seen in play the same day: a rule's cast drains the follower's magicka. Still to watch on the magicka bar: a dual cast taking the dual cost, a stream (Flames) its cost each second and stopping at empty, two followers casting on the same tick each losing their own spell's cost. The load line `a follower's cast through their record pays its magicka` says the hook is in. Still to see: whether a cast the engine refuses leaves the lease to its deadline as read.
- **Deferring to the AI's own cast, in play.** A cast rule waits (verdict "casting") while the follower is mid-cast on a spell of their own, instead of interrupting it (2026-09-04, Lightning Bolt on "magicka above half" cut off every spell Marcurio began). The risk was the other way: an AI that casts back to back never lets the rule through. Since 2026-09-22 (`wip-scoring`) the AI's own spells stand down while a rule waits so or holds a cast record (`dev/COMBAT_AI.md` "What we change"), so the AI finishes the cast in hand and starts no other. To see: `[ai] ... has a rule waiting on their own cast`, then the rule firing within a cast's length, and no lease ending unfired while the AI casts.
- **The AI's attack choice, in play** (built 2026-09-22 on `wip-scoring`, `dev/COMBAT_AI.md` "What we change"). With the log at debug, a caster follower in a long fight: the `[ai]` lines give each attack spell's engine score, perks, recency and draw; the spell cast should vary, change no sooner than a cast plus 3 s, and a spell whose hostile effects' conditions spare the enemy (a paralysis on an automaton) should read "answered 0". Also: the weapon factor on a follower with Armsman (about 1.2 to 2); whether calling the perk entry point and condition checks from the AI's thread is safe (it is where the engine's own gates evaluate conditions, but not seen); and whether a spell zeroed while in hand is put away within a second or cast once more first.
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
- **Shout, in play.** Built 2026-09-05 on the Shout package with the shout itself in the package's input (`dev/ACTIONS.md` 7); not yet run. `bat ftmake` teaches Unrelenting Force; the rule is `IF target: any THEN target: Shout -> Unrelenting Force`. The voice recovery is read (`Actor::GetVoiceRecoveryTime`, per actor, NPCs included) and a shout inside it reports "recovering" instead of firing; not yet seen counting down in play. Open: whether a three-word shout's long animation wants the begin step-back's 3 s or more.
- **A power's Voice type for the lease.** The power's shared record reads Type Voice for the ~2 s of a lease, restored on release (`dev/ACTIONS.md` 7). Works; the clean alternative is runtime-created Voice spells carrying the power's effects, which needs the active-effect check taught which spell stood for which power. Only worth doing if the window ever shows.
- **Voice pins, in play.** Built 2026-09-05: the Magic tab's Equipped cell for a power or shout readies it in the voice slot, pins it, or puts it away, like a hand cell; one voice pin sets every other power and shout aside with the "<x> is pinned" tooltip; the watchdog puts a pinned one back. Not yet run. Not covered: the combat AI's own shout entries are not in the score hook (only weapon and spell entry classes are), so an AI that shouts its own shout mid-fight is put back by the watchdog a tick later rather than kept from it.
- **Make-room unequips removed, in play.** 2026-09-05: when a pin displaces another, only the book changes and the engine's equip does the taking-off; the explicit unequip that followed was a leftover from the prevent-removal flag and, for the voice, undid the new equip. To watch for once each: weapon over weapon in one hand, spell over spell, armour over armour, a spell into a hand holding a weapon, a two-hander over sword and shield. If any leaves the old thing on, that case gets its unequip back.
- **Night Eye on the player.** Vanilla: `magicNightEyeScript` applies its image-space modifiers whoever the effect's target is, so a follower's Embrace of Shadows or Night Eye tints the player's screen. Not ours to fix unless it grates; the fix would be a script override that skips the modifiers when Target is not the player.
- **Summon and Corpse, in play.** Built 2026-09-05 (`dev/CONDITIONS.md` 6a); not yet run. `bat ftmake` grants Conjure Flame Atronach and Reanimate Corpse; kill a bandit from `bat ftman` for a corpse the spell will take. Rules: `Self: Summon none -> Self: Cast Conjure Flame Atronach`, and `Corpse: Highest level -> Corpse: Cast Reanimate Corpse`. Open: whether the UseMagic package aims at a dead actor; whether a raised corpse lands in the follower's `commandedActors` (the Summons tab and Summon: Active depend on it); and the 3000-unit corpse reach.
- **The follower's own spell numbers, in play.** 2026-09-05: the Magic tab's magnitude, duration, effect lines and description, and the Reanimate cap, are the follower's -- the record's number through the ModSpellMagnitude / ModSpellDuration perk entry points, which is how the engine makes the effect. Cost was already theirs. On a vanilla follower this changes nothing from the record for Fortify gear, and rightly: the two hidden perks that turn the Fortify values into anything (`PerkSkillBoosts` for enchantments -> cost, `AlchemySkillBoosts` for potions -> magnitude or duration, `dev/RESEARCH.md` 6) are on the player and the race presets only, so Fortify Destruction on a follower's robe does nothing in game and the panel shows nothing. Real perks do apply: a follower given Augmented Flames should show Firebolt above the record's 25, and the description should agree with the effect line. A mod that distributes the two perks to NPCs is picked up without a change here. The description is asked for with no parent so its tokens are left for us to fill; if it comes back already filled (the engine's, for the player), the parent-less call is not what it looked like.
- **Food and ingredients, in play.** Same build. Whether an NPC gets the effect of a food or an ingredient it eats through `EquipObject`, and what the settle time should be; the potion's 3 s is used meanwhile.
- **Any buff food, in play.** Built 2026-09-16 (`dev/ACTIONS.md` "Any: the rolled choices"); not yet run. Give a follower Beef Stew, Elsweyr Fondue and a stack of apples; `IF self: any THEN self: Food -> Any buff` should eat the stew and the fondue once each and never an apple, then read "every buff carried is already up" for twelve minutes. Watch `Buffs` against a load order's own foods, and whether the roll's spread looks even over a long fight.
- **Is Muffle worth leaving in as a buff?** `Buffs` counts it (a PeakValueModifier on MovementNoiseMult), so an "any" can roll a Muffle food or potion. It does something for a sneaking follower and nothing in a straight fight; if it turns out to waste bottles mid-combat, it goes out beside Waterbreathing, by actor value.

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
  line now means the detour missed a path, and prints the hand state.

- **The new conditions, in play** (dev/CONDITIONS.md 8): which of
  `kParalyzed` and the archetype flips first; how long `staggered` holds;
  whether the hit event fires for cloaks, hazards and concentration ticks;
  logged armour figures for a fight's enemies against the estimated tiers.
- **Attack, in play** (dev/ACTIONS.md 6). Whether the standard target
  selector lets a written `targetHandle` stand: `Ally: Attacked by Ranged ->
  Attacker: Attack` against a bandit archer, and read the log for
  "already fighting them" on the next tick, or the rule re-firing every
  two seconds. If it snaps back, the fallbacks are a selector vtable hook
  or a UseWeapon package. Also whether a hit event's `projectile` is set for
  every arrow and bolt, and never for a thrown or melee hit.
- **Power attack and bash, in play** (`dev/ATTACK.md` "What was built"). In a fight, `rule.resolved` for `power attack`, `bash` and `power bash`: `made` against `not-made`; for a power attack, the attack the hands make (`attackEvent`) and that the follower does not go on to a second one; for a bash, `bashS` against `staminaAtRequest` and `staminaAtEnd`: the engine's own bash holds the bash state about half a second and costs `fStaminaBashBase` 35, where the two shortest made on 2026-09-15 held it 0.09 s. A power bash is measured and working (`dev/ATTACK.md`); a plain bash has not been read the same way. A sword and shield follower (Bash, Power Attack) and a follower with a spell in the right hand and a dagger in the left (the left-hand power attack) cover the hands.
- **The Settings requirements, in play** (built 2026-09-15, `dev/PROFILES.md` "The settings record"). Three switches under Customize: dual wield combat style (on), dual casting perks (off), power bash perk (off). Verify each way round: with the perk requirements off, a follower with no perk is offered Dual Cast and Power Bash and both fire; with them on, neither is offered and a written rule for either reads as unavailable, the log saying `no-perk` or `cannot-dual-cast`. With the dual wield requirement off, a follower whose style forbids it keeps a weapon in each hand; turning it back on takes nothing off by itself (the AI decides again). Then save, reload, and check the three read back as they were left, along with the switch over all followers, and that a save made before the record existed loads the defaults (everything on but the two perk requirements). A follower's own switch was already saved in their own record; verify it still is.
- **Cast on a chosen target, in play.** A targeted spell now goes at whom
  the rule aimed it (the Then cascade's first level: self, the ally or
  enemy the condition matched, the player, a named follower, the target,
  the attacker). Verify Heal
  Other lands on the hurt ally and on the player through the package's
  Target input, and what an aimed stream (Healing Hands) does at a moving
  ally.
- **Group subjects, in play.** Allies and enemies are sensed by
  definition (dev/CONDITIONS.md 6): the player and the teammates, and
  whoever is in combat and hostile to the player. Verify against the log's
  once-per-fight "allies / enemies" line that a dead enemy drops out and a
  distant hostile is not counted before the player is in its fight.
- **Tactics in the save, in play.** Written by the SKSE save callback
  and taken back at first sight after a load (`dev/PROFILES.md`), built
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
- **Shareable named profiles.** Profiles currently live in the SKSE co-save.
  Copying a list between followers and importing or exporting a profile for
  sharing remain future work over the same JSON format.
- **Named-potion effect check.** `DrinkPotion` has no "already in effect"
  test; only its per-potion cooldown spaces it. The strongest-of-a-kind
  actions do check the restore effect.

## Panel

- **Custom skill trees, in play.** Built 2026-09-15 on `wip-breakdown`, not yet run: Custom Skills Framework's trees (Exit-9B's SKSE plugin; a JSON file per mod in `Data/SKSE/Plugins/CustomSkills`) read by `game/CustomSkillsFramework` on first use, parsed and ordered in `core/CustomSkills`, and listed on the Skills tab under Other Skills with the perks held, their perks kept out of Other Perks and their pages naming the skill. To watch in Nordic Souls: Stormcrown's Dragonborn, Camping Plus Plus (whose name is a translation key) and The Dragon Cult's Priesthood; the order a tree lists in (by depth, then left to right as the menu draws it, which is descending x in both the framework's files and the vanilla records); the log's `customskills` lines, one per tree loaded; a file that will not read is left out with a warning. Vanilla trees now break ties of requirement by the same order.
- **The player's page, in play.** Built 2026-09-13 on `wip-player-stats`: Follower Tactics > Player, the sheet's six tabs over the player, read when the panel opens and after each click, the equip cells equipping and unequipping only (no pin, no ban). Not yet run. The page is the check `dev/MODIFIERS.md` asks for: its Damage, Armor, Cost and Magnitude figures should equal the game's own inventory and magic menus, and an Other line on a hover is a formula that is wrong for the player (damage and spell cost branch to the player's settings; the armour curve is the engine's own), except on a value's hover, where Other is also whatever a script or another plugin wrote straight into the value (Blade and Blunt's injuries, seen 2026-09-14). To watch: the Skills tab lists the tree perks taken, and Other Perks a stone's or a quest's perk (found through the load order for the player, not the base record); the Shouts chip lists the shouts learned (read off the base record's spell list, where the player's are expected to be; unverified); the Health, Magicka, Stamina and Carry Weight hovers, where a level-up's increase may show as Other; the log's "player page built in N ms" with a large bag. With `FreezeTimeOnMenu = false` the page stays as it was at the open.

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

- **Structured log — built 2026-09-09; game events, the in-memory list and the session archive built 2026-09-14; none of it verified in play.** `dev/LOGGING.md` is the machinery, `dev/EVENTS.md` the events and its own "To verify in play" list. What is left is a session with the game up: that the ini is found under MO2's virtual file system, that the previous session's pair is archived and the new one opens, and each item on EVENTS.md's list.
- **The Logs tab** that reads the in-memory events (`log::RecentEvents`): not built. `dev/EVENTS.md` says why it reads memory rather than the file.
- **A cast's `interrupted` outcome** (`dev/EVENTS.md`, "How the harder ones are known"): needs the begin-cast flag tied to our spell, and checking in play.
- **The shipped ini's `level = debug`** contradicts the default it documents (`info`); left as it is while the dist zip is being tested.
- **Recruiting from the console.** `cqf DialogueFollower SetFollower <refid>`
  should fill the alias without the dialogue; unverified.
- **The player's hands given back after a cast, in play** (fixed 2026-09-19, `wip-game-test-2`). Dual wield two copies of one weapon -- two Iron Daggers -- and have a rule cast a two-handed spell or a dual cast, which borrows both hands. Both daggers should come back. Before the fix only one did: the restore compared the form it had just put back, which is one object for both copies, and skipped the second. A greatsword should still come back once, not twice.
- **The later extractions of 2026-09-19, in play** (`wip-game-test`, `wip-game-test-2`; the ledger in `dev/TESTING.md`). Like the five below, each moved a decision into core without a behaviour change, verified by the tests and a build, never by a run. The events log is the check: every `reason` on `rule.resolved` is one `dev/EVENTS.md` lists, since the extracted steps produce the same words. To see: a follower's heal cast resolves `spell fired`, a stream `stream ended`, a shout `shout fired` and a power `power fired` (the leases); a power attack resolves `power attack made` with `distanceAtRequest` near what it was before (the reach); a sword-and-shield follower's bash resolves `bash made`; the player's own cast, dual cast, shout and power resolve as in `dev/PLAYER.md` (the player cast); a rule pinning a banned dagger in a fight holds it, and `ban.enforced` with `by: fight-end` takes it off the tick the fight ends (the watchdog); `pin.restored` puts back what was pinned before the fight; the Inventory tab shows a poisoned and a tempered copy as rows of their own and the plain stack once, with the pin marker on the worn copy only (the partition and the marks); a Use power rule on a greater power used today reports it used, and a Become Ethereal rule waits while the shout holds (the spells); a newly recruited party lists alphabetically in the menu (the slots).
- **The five extractions of 2026-09-19, in play** (`wip-game-test`; `dev/TESTING.md` "The game layer without Skyrim"). Each moved a decision into core and switched the game over without a behaviour change, and none has been run in game since. To see: a save loads with its tactics and settings (`the save holds tactics for N follower(s)` in the log, the rules on the page); the ini still sets the level (the banner names it); a fight begins and ends with `combat.entered` and `combat.left` once each, and the idle list runs after; a follower with the combat list switched off runs the idle list after a fight (the fix); the panel's click on a row, on the plain stack, and on a copy worn in the other hand still equips the copy clicked (`equip ... list [...]` at debug names it); a pinned dagger the engine moved comes back across.
- **Package tests.** The cast records and leases have no unit tests because they
  touch the game. A seam that lets the tick and release logic run against a
  fake actor would cover the release paths.
- **What the core tests never reach** (coverage, 2026-09-09; `.\tools\build.ps1 -Preset core-cov -Coverage`, line-by-line in `build\core-cov\coverage\html`). 98% of `src/core` lines; `Profile.cpp` is complete. What is left is by construction: the resolver's branch for an enemy's attacker (the validity matrix never lets that pair through), the `default:` arms of exhaustive switches, and the `Verdict` words' final `"?"`. Nothing worth a test.

## Toolchain

- **Version-bound addresses, before shipping** (`dev/VERSIONS.md`). Every Address Library ID, vtable slot and engine layout read from 1.6.1170 alone: each gets its Special Edition (and VR) counterpart read and checked, or the feature it serves says plainly that it is AE-only.

- **Upstream to CommonLibSSE-NG** what `dev/COMMONLIB.md` has not reported: the `GetTargetActor` bug (its draft is `dev/bugs/commonlibsse-ng-gettargetactor.md`), the missing spell and shout unequips, the combat inventory's field order, and (2026-09-24) the inventory weight reset, the perk rank change event, and `UseSkill`'s missing fourth argument. Each entry there says what the PR would add, with both halves of every ID.

- **CommonLibSSE-NG migration** to alandtse `ng` (`dev/COMMONLIB.md`): brings
  `ForceRefTo`, and re-check the `Skyrim.INI` log-directory quirk.

## Left from the 2026-09-11 code review

The review's bugs, dead code, duplication and wording items are done (the commits of 2026-09-11), and so is the `PanelState` consolidation it asked for (2026-09-16): one state per follower in one map, the chip and filter each list shares across pages in a `ListView` beside it, and the pending tab switch moved out of the Inventory tab's state to the page's own, where its four writers belong. What it also asked for, and is not done, each with why:

- **Verify in play what the review could not.** The cast sink's atomics, the score hook under its lock, the equip detour's one-copy rule, the per-hand poison and charge readings, and calibration refusing an inline canary all compile and lint clean and have not been run in a fight.
- **Split `UI.cpp` (rule editor, sheet pages, widgets) and `Sensors.cpp` (snapshot, sheets), and move the spell math out of `Inventory.h`.** Mechanical but large, and only verifiable in play; the shared pieces both halves would need are in `game/Sheet.h` now, so the split is a move.
- **The engine's own name tables** for the condition functions and the perk entry points (`SCRIPT_FUNCTION::GetFirstScriptCommand`, `BGSEntryPoint::GetEntryPoint`) in place of `ConditionNames.inc` and `kEntryPointNames`. Needs a play session to confirm the engine's strings match the Creation Kit's wording.
- **Smaller repeats left in `src/game`:** the cast and shout request prologue in `Packages.cpp` (a `ResolveTarget` and a `ClaimSlot`; the `skyrim_cast` -> `FindInputUID` -> `InputByUID` chain six times), the spells-shouts-scrolls enumeration in the snapshot against the panel's scans, the Fortify-factor block three times, the Speed row's own `ValueNote` threshold, `%08X` inline against `HexId`, the two `ReplaceNoCase` copies, and the perk-description read four times. Each is small; none has cost anything yet. The panel's share of this list is done (2026-09-16): the filter-sort-tiebreak is `SortRows`, the chip rows `DrawCategoryChips`, the detail headers `BackButton` / `DetailName` / `DetailSubtitle`, and the Order cell `OrderButtons`. One item named there was never a repeat: the stat rows are six calls to one `DrawStatRow`, which is what they should be.
- **The table drawn in pieces, twice.** The rule table (`beginPiece` / `endPiece`) and `DrawSections` both split a table into pieces around a drawer, for the same reason and by the same means, and each says so in a comment pointing at the other -- while sharing no code. Left out of the 2026-09-16 dedup pass deliberately: unlike the four repeats above it is structural rather than mechanical, about a hundred lines on each side, and what makes it hard is the seam -- outer borders drawn by hand down the drawer's sides, striping counted across pieces, no vertical spacing between one piece and the next -- which no test can check and only the eye can, in game.
- **`/we4062` with no `default:` on switches over the rule enums,** so a new `ActionKind` cannot be half-added. Every classifier is one function now, which covers the case the review found; the compiler check would cover the next one.
- **`Profiles.cpp` and `Tactics.cpp` include each other;** the profile assembly in `LoadIfNew` and `ProfilesToSave` would move into `Profiles.cpp` with the identities.
- **`Downgrade-Skyrim.ps1 -Step check` and `check_install.py` check the same things twice.**
