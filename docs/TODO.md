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

- **Equipment swapping, and pins.** There is no "equip item" action yet. When
  there is, it meets the Inventory tab's pins: a rule that swaps to a bow
  should not fight a pinned sword, and the watchdog should not undo a swap
  the rules made. Likely shape: a rule's swap releases the pin on what it
  displaces, and re-pins it (or not) when the rule's condition lapses.

- **Group subjects.** The snapshot carries one enemy, the one she is
  engaging, and no allies. "Any enemy below 30% health" and "ally in
  bleedout" need the combat group read in full (`Snapshot::enemies`,
  `Snapshot::allies`).
- **Rules do not survive a restart.** Rule sets live in memory. Phase 2's
  JSON load/save (the shareable profile format) and per-follower profiles
  are the fix; wire names are already stable for it.
- **Named-potion effect check.** `DrinkPotion` has no "already in effect"
  test; only its per-potion cooldown spaces it. The strongest-of-a-kind
  actions do check the restore effect.

## Panel

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
