# What is still to do

Kept short and current. Each item says what is missing and, where known, what
it takes. Measured facts behind them are in `docs/MAGIC.md`; the phase plan is
in `docs/PLAN.md`.

## Casting

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
- **Our own quest and aliases.** Today the packages ride the vanilla
  `DialogueFollower` alias's combat-override list, so only a follower
  recruited through `SetFollower` is covered. A FollowerTactics quest with
  eight aliases, filled for any teammate we manage, removes that dependence
  and covers every framework. Blocked on `BGSRefAlias::ForceRefTo`, which is
  in alandtse's CommonLibSSE-NG but not CharmedBaryon 3.7.0 -- see
  `docs/COMMONLIB.md`. Until then, NFF is one line in `kOverrideLists`.

## Rule engine

- **Equip rules, in play.** The four equip actions pin through the same
  book as the Inventory tab, and the evaluator is tested; what is not yet
  verified in game is a pin landing mid-fight when she holds something
  else -- whether the AI switches to it, or is only kept from switching
  away once she has it -- and what the game does after "Equip armor: None"
  takes pinned pieces off (her outfit may not come back until a cell
  change).
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
- **Cast on a chosen target, in play.** A targeted spell now goes at whom
  the rule aimed it (the "On" choice in every action's menu: whoever
  matched, self, the player, the target, their attacker). Verify Heal
  Other lands on the hurt ally and on the player through the package's
  Target input, and what an aimed stream (Healing Hands) does at a moving
  ally.
- **Group subjects, in play.** Allies and enemies are sensed by
  definition (docs/CONDITIONS.md 6): the player and the teammates, and
  whoever is in combat and hostile to the player. Verify against the log's
  once-per-fight "allies / enemies" line that a dead enemy drops out and a
  distant hostile is not counted before the player is in its fight.
- **Rules do not survive a restart.** Rule sets live in memory. Phase 2's
  JSON load/save (the shareable profile format) and per-follower profiles
  are the fix; wire names are already stable for it.
- **Named-potion effect check.** `DrinkPotion` has no "already in effect"
  test; only its per-potion cooldown spaces it. The strongest-of-a-kind
  actions do check the restore effect.

## Panel

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

- **Structured log.** A JSON-lines file beside `FollowerTactics.log` for the
  events worth querying (arm, fire, release, rank), so `jq` can answer "did
  she cast twice" without reading prose.
- **Recruiting from the console.** `cqf DialogueFollower SetFollower <refid>`
  should fill the alias without the dialogue; unverified.
- **Pool tests.** The package pool and lease have no unit tests because they
  touch the game. A seam that lets the tick and release logic run against a
  fake actor would cover the release paths.

## Toolchain

- **CommonLibSSE-NG migration** to alandtse `ng` (`docs/COMMONLIB.md`): brings
  `ForceRefTo`, and re-check the `Skyrim.INI` log-directory quirk.
- **The live ESP is the MO2 copy.** houseCARL writes
  `MO2/mods/FollowerTactics/FollowerTactics.esp`; `esp/FollowerTactics.esp`
  is the versioned copy and has to be copied by hand after every edit.
