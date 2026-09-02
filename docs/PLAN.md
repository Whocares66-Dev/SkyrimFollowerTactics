# Skyrim Follower Tactics — Implementation Plan

A Dragon Age: Origins-style tactics system for Skyrim SE/AE followers: an ordered,
player-authored list of `IF <condition> THEN <action> ON <target>` rules, evaluated
per follower, editable in-game.

**Decisions locked (2026-09-01):**

| Decision | Choice |
|---|---|
| Target runtime | Downgrade to **1.6.1170**, block Steam auto-update |
| Implementation | **Hybrid** — C++ SKSE rules engine + thin ESP/Papyrus shell |
| UI | **In-game ImGui** (SKSE Menu Framework) + **JSON rules on disk** |
| Goal | **Prototype first** — prove the risk, defer release engineering |

---

## 1. The core technical constraint

This is the finding that dictates everything else, and it is well supported:

> Skyrim's moment-to-moment combat decision loop — target selection, when to attack,
> when to block, when to flee, when to drink a potion, which spell to cast — is
> **hardcoded in native engine code and is not exposed to Papyrus at all.**

Papyrus gets you exactly two levers over follower combat behavior:

1. **CombatStyle (CSTY) records** — a static bundle of probability multipliers
   (offensive/defensive mult, group offensive, equipment score weighting, circle/fallback,
   flank distance, strafe, bash/dual-wield toggles). No conditional logic, no flee threshold,
   no "avoid AoE" field. `ActorBase.SetCombatStyle` swaps the whole record; there is no
   per-field runtime setter in Papyrus.
2. **AI package stack** — quest-alias packages outrank the actor's own package list, and
   a dedicated **Combat Override** package list exists. Package conditions can read live
   Papyrus values via `GetVMQuestVariable` / `GetVMScriptVariable` on `Conditional`-flagged
   int/float/bool properties.

Everything else is off-limits from script. There is no native "health dropped below X%"
event — you must poll. And Papyrus polls inside a shared **~1.2 ms/frame** VM dispatch
budget (`fUpdateBudgetMS`, default 1.2) across every script in the entire load order, on a
VM whose design target was 2011 mid-range hardware.

This is why every mod that achieves genuine conditional NPC behavior does it in C++:

| Mod | Technique | Status |
|---|---|---|
| [NPCsUsePotions](https://github.com/muenchk/NPCsUsePotions) | Native threshold evaluation on health/magicka/stamina + cooldowns; **already follower-aware** | v4.2.0, Aug 2026, active |
| [Puppeteer](https://github.com/NHK-512/Puppeteer-SKSE) | Rewrites CombatStyle multipliers at runtime to assign group roles | v1.4.0, May 2026, active |
| [Project-NSAI](https://github.com/LeoneKingzz/Project-NSAI) | Scored situational assessment → discrete stance switching. **Closest existing architecture to a rules engine.** | v1.4.1, Apr 2026, active |
| [Ultimate NPC Dodging](https://github.com/LeoneKingzz/Ultimate-NPC-Dodging-PoiseBuild) | Reflex score + incoming-attack geometry → dodge | active |
| [SCAR](https://github.com/max-su-2019/SCAR) | Havok behavior-graph hooks for combo AI | MIT |
| [Combat Pathing Revolution NG](https://github.com/clayne/Combat-Pathing-Revolution-NG) | Assembly hooks into combat pathing | MIT |

**Design principle that follows: bias the native AI, don't puppet the actor.**
DA:O tactics operate at exactly the granularity Skyrim can support — "use a potion",
"use this ability", "target the caster", "hold position" — not frame-level motor control.
Rules that need frame-level control (dodge this swing, interrupt that cast) are Phase 5+,
and some are behavior-graph work, not plugin work.

## 2. Prior art: the gap is real

Searched Nexus, GitHub, and the web for an existing condition→action tactics/gambit editor
for Skyrim followers. **Nothing found.** The space splits cleanly into two non-overlapping
halves and no one occupies the intersection:

- **Follower frameworks** — [NFF](https://www.nexusmods.com/skyrimspecialedition/mods/55653)
  (v2.8.6b, Apr 2024, closed source, most compatible), AFT (dormant since 2017), EFF
  (dead since 2018, author account expired). All Papyrus + MCM. All top out at picking one
  of ~5–10 static combat-style presets plus a role toggle. None expose conditional rules.
- **Combat AI plugins** — the C++ mods in the table above. Real conditional logic, but no
  player-facing editor and no concept of "follower" as distinct from "any NPC".

Nearest thing to prior art by name, and **not** actually prior art:
"Sidekick's Tactics SSE" is a single scripted ward set-piece; "SkyTactics" is randomized
combat-style assignment via SkyPatcher; the several "Dragon Age Followers" mods are
character ports. [SeverActions](https://github.com/Severause/SeverActions) /
[SkyrimNet](https://github.com/MinLL/SkyrimNet-GamePlugin) is the most sophisticated
adjacent project but is LLM-inferred behavior, a different paradigm from deterministic
player-authored rules — worth one line in the eventual mod description to avoid confusion.

Caveat on the negative result: r/skyrimmods returned 403 to automated fetch, so community
discussion threads were not directly searched. Treat this as "extensively searched, none
found" rather than proof of absence.

---

## 3. Architecture

```
                    ┌─────────────────────────────────────┐
                    │  ImGui UI (SKSE Menu Framework)     │  editor
                    │  follower list │ rule table │ debug │
                    └──────────────┬──────────────────────┘
                                   │ reads/writes
                    ┌──────────────▼──────────────────────┐
   profiles/*.json ─┤  RuleSet (pure C++, no RE:: types)  │  ← unit testable
                    │  Rule{ cond, target, action, args } │
                    └──────────────┬──────────────────────┘
                                   │ evaluated against
                    ┌──────────────▼──────────────────────┐
                    │  Snapshot (POD, no RE:: types)      │  ← unit testable
                    │  self AVs, target AVs, distances,   │
                    │  enemy count, ally states, flags    │
                    └──────────────┬──────────────────────┘
              built by │           │ produces
                    ┌──▼───────────▼──────────────────────┐
                    │  Engine adapter (all RE:: lives here)│
                    │  registry · scheduler · sensors ·    │
                    │  action dispatch                     │
                    └──────────────┬──────────────────────┘
                                   │
                    ┌──────────────▼──────────────────────┐
                    │  ESL-flagged ESP: abilities, AI      │
                    │  packages, keywords, combat styles   │
                    └─────────────────────────────────────┘
```

The horizontal line matters more than any other decision in this document. **No `RE::` type
crosses into `Snapshot` or `RuleSet`.** There is no headless test harness for Skyrim; the
only way to get automated tests is to make the interesting logic not need the game. Rule
matching, priority resolution, cooldown arithmetic, target resolution, and JSON
serialization all live above the line and get Catch2 tests running in CI. Below the line is
thin, imperative, and verified by playing.

### 3.1 Follower registry

Who is under tactics control. Use the native **`Actor::BOOL_BITS::kPlayerTeammate`**
flag (`1 << 26`, confirmed present in CommonLibSSE's `RE::Actor` BOOL_BITS enum; read it off
the actor's bool bits — note this is a flag, not a named `IsPlayerTeammate()` method, so
confirm the exact accessor on your CommonLibSSE-NG version). Every follower framework —
NFF, AFT, EFF — ends up setting this flag, so keying off it integrates with all of them for
free without taking a dependency on any. Layer on:

- opt-out list by FormID (some teammates are pack mules, not combatants)
- optional `CurrentFollowerFaction` cross-check for vanilla-only mode
- explicit "managed" flag stored in the co-save

Registry is rebuilt on `OnCombatStateChanged`-equivalent events and on cell change, not per
tick. Cap the managed set (start at 8) and log when it is exceeded.

### 3.2 Scheduler

Do **not** hook `Actor::Update` in the prototype. Run an own scheduler on
`SKSE::GetTaskInterface()`, ticking each managed follower on a fixed interval
(default 150 ms, configurable 50–500 ms), **staggered** so N followers do not all evaluate
on the same frame. In-combat and out-of-combat intervals are separate settings; out of
combat can drop to 1 s.

Budget target: total tick cost across 8 followers under 0.5 ms/frame amortized. Instrument
it from day one — a rolling per-tick microsecond histogram exposed in the debug UI, not
guessed at.

### 3.3 Snapshot (the blackboard)

Built once per follower per tick, plus a shared world snapshot built once per tick for all
followers. Fields, roughly:

- **self**: health/magicka/stamina current+max+pct, in combat, in bleedout, sneaking,
  detected, weapon drawn, equipped weapon type, equipped spell(s), active magic effects
  (as flags: on fire, frozen, paralyzed, silenced, poisoned), position, current package
- **target**: current combat target's AVs, distance, LOS, is casting, is power attacking,
  is blocking, actor type (undead/dwarven/dragon/humanoid), level delta
- **player**: AVs, in combat, sneaking, distance, is being attacked by N
- **allies**: count, lowest-health ally + its pct, any ally in bleedout
- **enemies**: count within R (8/16/32 units bands), nearest distance, count in melee range
- **inventory**: best available health/magicka/stamina potion tier, count; poisons; food
- **bookkeeping**: per-rule cooldown timers, last-fired rule index, global action cooldown

Every sensor is a separate function with a cost tag. Expensive sensors (LOS raycasts,
inventory scans) are cached with their own refresh interval and only computed if some
enabled rule actually references them — a rule set that never asks about poisons never
pays for scanning the inventory. This dependency-driven sensor activation is the main
performance lever and should be built in from Phase 2, not retrofitted.

### 3.4 Rule semantics

DA:O semantics, which are worth copying exactly because they are proven and players already
know them:

- Rules are an **ordered list**, evaluated top to bottom each tick.
- **First rule whose condition is true and whose action is currently available fires.**
  "Available" means: not on cooldown, target resolves, resource exists (the potion is in
  the inventory), and the action's capability check passes.
- If no rule fires, do nothing — the native AI keeps running. This is important: the system
  is an *override layer*, never a replacement. A follower with an empty rule set behaves
  exactly like vanilla.
- One action per tick per follower, plus a short global cooldown (default 500 ms) to stop
  a rule from thrashing.

A rule is `{ enabled, condition, condition_args, target, action, action_args, cooldown_ms }`.

Conditions (v1 set): `Always`, `SelfHealthPctBelow`, `SelfMagickaPctBelow`,
`SelfStaminaPctBelow`, `AllyHealthPctBelow`, `PlayerHealthPctBelow`, `EnemyCountAbove`,
`EnemyWithinDistance`, `TargetIsActorType`, `TargetHealthPctBelow`, `SelfHasMagicEffect`,
`SelfInBleedout`, `InCombat`, `PlayerSneaking`.

Targets (v1): `Self`, `Player`, `CurrentTarget`, `NearestEnemy`, `FarthestEnemy`,
`LowestHealthEnemy`, `LowestHealthAlly`, `EnemyAttackingPlayer`, `NearestCaster`.

### 3.5 Actions — tiered by how much they fight the engine

**Tier A — reliable, ships in the prototype.** These either set state the native AI reads,
or perform a discrete one-shot the AI does not contest.

- `DrinkPotion(kind, tier)` — the highest-value action and the one to build first.
  The confirmed C++ route is the native equip manager, which is what makes an actor actually
  consume an `AlchemyItem`:
  ```cpp
  RE::ActorEquipManager::GetSingleton()->EquipObject(
      actor, potion, /*extraData*/ nullptr, /*count*/ 1, /*slot*/ nullptr,
      /*queueEquip*/ true, /*forceEquip*/ false, /*playSounds*/ true, /*applyNow*/ false);
  ```
  Use this, not the Papyrus `EquipItem`-on-a-potion trick, which has a documented bug where
  effects randomly re-apply long after expiry. Cross-check parameter choices against
  NPCsUsePotions, which is confirmed to be a CommonLibNG C++ plugin supporting SSE/AE/VR.
- `SetCombatStyle(style)` — Puppeteer's technique. Swap in one of a handful of CSTY records
  shipped in the ESP (aggressive / defensive / ranged / support / flanker).
- `SetAggression(level)` / `SetConfidence(level)` — AV writes; controls whether the follower
  engages on sight and whether it flees.
- `EquipLoadout(set)` — force weapon/spell selection. Note this is *advisory*: the AI can
  re-derive equipment on its own schedule. `preventRemoval` locks the slot but not the AI's
  broader weapon-choice logic.
- `ApplyAbility(spell)` — add/remove a hidden constant-effect ability from the ESP that
  shifts AVs. This is the general-purpose escape hatch for "make the follower tankier /
  faster / stealthier right now" without touching the combat loop.
- `PushPackage(package)` / `ClearPackage()` — package override for `Flee`, `KeepDistance`,
  `GuardPlayer`, `HoldPosition`. Note `EvaluatePackage` is documented as unreliable for
  interrupting an in-flight Travel package — for combat overrides this matters less, but
  expect flakiness and log when a pushed package does not become the current package.
- `StopCombat()` / `StartCombat(target)` / `SetAttackActorOnSight(bool)` — hold fire /
  free fire.
- `MoveToOffset(anchor, distance)` — `KeepOffsetFromActor` for spacing.

**Tier B — works but contests the AI. Prototype the technique before promising the feature.**

- `CastSpell(spell, target)` — there is **no clean animated forced cast** from script.
  `Spell.Cast`/`RemoteCast` fire instantly with no animation and the CK wiki explicitly
  calls them unsuitable for actor casting. The C++ route is
  `RE::ActorMagicCaster` / the actor's magic caster slots — **verify the exact API and its
  animation behavior in Phase 4 before designing UI around it.** Fallback: `EquipSpell` +
  a `UseMagic` package override + a caster-biased combat style, which controls *what* gets
  cast but not *when*.
- `UsePoison(poison)` — NPCsUsePotions already does this, follower-aware. Read its source.
- `Shout(shout)` — likely needs a package or behavior event; unverified.

**Tier C — deferred, possibly out of scope.**

Forced dodge, forced block, forced power attack, interrupt-on-enemy-cast, formation
positioning. These need behavior-graph work (Nemesis/OAR, as Ultimate NPC Dodging does) or
assembly hooks (as Combat Pathing Revolution does). Do not put them in the v1 UI.

**The action registry carries a capability flag per action.** The UI greys out actions whose
capability check fails on the current runtime, rather than letting a player author a rule
that silently never works. This is the single best defense against the failure mode where
someone writes twelve rules and concludes the mod is broken.

### 3.6 Storage

Two separate stores, deliberately:

- **Rules → JSON on disk**, `Data/SKSE/Plugins/FollowerTactics/profiles/*.json`.
  Parsed with nlohmann/json. Shareable, diffable, hand-editable, version-stamped with a
  schema version and a migration path from day one. Not in the save, so a profile survives
  save-game churn and can be posted on a forum.
- **Assignments and runtime state → SKSE co-save.** Which actor uses which profile, plus
  cooldown timers. Keyed by FormID and **every stored FormID must go through
  `SerializationInterface::ResolveFormID` on load** — this is the single most common cause
  of "my mod's data vanished after I reordered my load order." Entries that fail to resolve
  are dropped with a log line, not silently kept.

### 3.7 UI

[SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) v3.14.1
(28 Aug 2026, added Skyrim 1.7 support, ~260 dependent mods, MIT SDK). Register one section.

Layout:

- **Left**: managed follower list, with a live health/magicka/stamina readout and the
  currently-assigned profile.
- **Right**: ImGui table of rules. Drag-to-reorder rows, enable checkbox, combos for
  condition / condition arg / target / action / action arg, per-row cooldown.
- **Right, bottom — the debug column, and the most valuable single feature here.**
  Per rule, live: `condition value` (e.g. "health 0.72 vs threshold 0.50"), `last fired`,
  `blocked by` (cooldown / no target / no potion in inventory / capability unsupported).
  DA:O never had this, and without it, authoring rules against an opaque engine is guesswork.
- Global: tick interval, per-follower enable, tick-cost histogram, profile save/load/clone.

Note SKSE Menu Framework defaults to freezing time while its menu is open — for authoring
that is correct, but the debug readout is then static. Provide a non-freezing mode so rules
can be watched live during a fight.

### 3.8 ESP

ESL-flagged. Record budget is nowhere near the 2048 limit. Contents:

- 5–8 `CSTY` combat styles (the `SetCombatStyle` action's palette)
- ~10 `SPEL`/`MGEF` hidden abilities (the `ApplyAbility` palette)
- ~6 `PACK` AI packages (flee, keep distance, guard player, hold position, use magic)
- a `QUST` with reference aliases if package pushing turns out to need alias priority
- keywords for opt-out tagging

Note the CK bug where `Conditional` properties on **alias-hosted** scripts frequently fail
to populate in the `GetVMScriptVariable` dropdown — type the variable name manually. Only
relevant if the design ends up needing Papyrus-driven package conditions; the C++-first
approach mostly routes around it.

---

## 4. Phases

### Phase 0 — Environment (½ day)

1. **Downgrade to 1.6.1170.** The vanilla downgrade patcher is discontinued; use
   [SDT – Skyrim Downgrade Tool](https://www.nexusmods.com/skyrimspecialedition/mods/188916)
   or [Reliquary](https://www.nexusmods.com/site/mods/2188). Then block Steam auto-update
   (set `appmanifest_489830.acf` read-only, or set the game to "only update when launched"
   and never launch from Steam). **Verify `SkyrimSE.exe` version before and after.**
2. **SKSE64 2.2.8** (the build for 1.6.1170 — *not* the current 2.3.1, which targets
   1.7.104), Address Library "All in One v13", Crash Logger, and an MO2 profile dedicated to
   dev with a minimal load order (SKSE + Address Library + SkyUI + SKSE Menu Framework +
   this mod). Full step-by-step in `docs/SETUP.md`.
3. Creation Kit (Steam appid 1946180) +
   [CKPE](https://github.com/Perchik71/Creation-Kit-Platform-Extended). Set
   `bAllowMultipleMasterLoads=1` and `sScriptSourceFolder=".\Data\Scripts\Source"`.
   Run CK through MO2; route its output to the dev mod folder, not `overwrite`.
4. VS2022 + CMake + vcpkg + CommonLibSSE-NG. Start from
   [SkyrimDev/HelloWorld-using-CommonLibSSE-NG](https://github.com/SkyrimDev/HelloWorld-using-CommonLibSSE-NG)
   and set `SKYRIM_MODS_FOLDER` so a rebuild deploys the DLL straight into the MO2 dev mod.
5. Test scenario — **no Creation Kit needed**, it is four console batch files in `test/`
   (see `docs/TESTING.md`): `coc QASmoke` manually, then `bat ftsetup` / `ftspawn` /
   `fthurt` / `ftclean`. Plus a saved "clean dev save" parked in the test cell.
   **Discipline: any structural script/plugin change means a fresh save.** Papyrus bakes
   script *instance state* into saves; changing a script's shape and reloading an old save
   produces orphaned instances and unreliable test results.
6. `git init` here; the ESP, scripts, C++ source, and profiles all version-controlled.

**Exit criterion:** a DLL that logs "hello" to
`Documents/My Games/Skyrim Special Edition/SKSE/FollowerTactics.log`, built and deployed by
one command, plus a save you can reload in under 30 seconds.

### Phase 1 — Risk spike: one hardcoded rule (2–4 days)

No UI. No JSON. No rule engine. Hardcode exactly:

> if the player's current follower's health < 50%, drink the best health potion in their
> inventory, at most once every 10 seconds.

This single vertical slice exercises every risky subsystem at once: follower identification,
the scheduler, actor value reads, inventory scan, and — the hard part — actually making an
NPC consume a potion reliably. Read [NPCsUsePotions](https://github.com/muenchk/NPCsUsePotions)
source before writing this; it has solved exactly this problem and is follower-aware.

**Exit criterion:** stand in a test cell, damage the follower, watch them drink. Log line
per evaluation. Tick cost measured, not assumed.

**If this takes more than a week, stop and reconsider the whole project** — everything
downstream is easier than this, and if potion-drinking can't be made reliable, the marquee
DA:O rule ("health < 50% → use strongest health potion") does not work and the concept needs
rethinking.

### Phase 2 — Rule engine (1 week)

- `Snapshot` POD + sensor functions with dependency-driven activation
- `Rule` / `RuleSet` + evaluator, all above the `RE::` line
- Condition, target, and action registries with capability flags
- JSON load/save with schema version + migration
- **Catch2 test suite on the pure logic**, running in CI with no game involved.
  This is the part that makes the project maintainable; it is also the part that is easy to
  skip and impossible to retrofit cheaply.
- Still no UI — profiles hand-written as JSON.

**Exit criterion:** three hand-written JSON rules of different shapes all firing correctly,
proven by log; test suite green.

### Phase 3 — ImGui UI (1 week)

SKSE Menu Framework integration, follower list, rule table with drag-reorder, and the debug
column. Build the debug column first, not last.

**Exit criterion:** author a working rule set from scratch, in game, without touching a file.

### Phase 4 — Action breadth (1–2 weeks)

Tier A actions completed; Tier B techniques prototyped and either promoted or documented as
unsupported. Co-save persistence with `ResolveFormID`. Per-follower profiles, cloning,
defaults per archetype (tank / healer / archer / mage).

### Phase 5 — Decide

At this point the prototype is a working mod. Then decide: keep it personal, or invest in
release engineering — NFF interop testing, load-order compatibility matrix, multi-runtime
CommonLibSSE-NG build for 1.7.x, MCM fallback for users who won't install a second DLL,
translations, packaging.

---

## 5. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Forcing an NPC to drink a potion isn't reliable | **Highest — kills the marquee feature** | Phase 1 exists solely to retire this risk. NPCsUsePotions proves it is solvable and is open source. |
| Bethesda ships another runtime patch | High | Already mitigated by pinning 1.6.1170 and blocking Steam updates. 1.7.99 and 1.7.104 landed a week apart in Aug 2026 and broke the ecosystem; this is not hypothetical. |
| CommonLibSSE-NG's last tagged release is v3.7.0 (May 2024) | Medium | Irrelevant at 1.6.1170, which v3.7.0 covers. Becomes the blocking issue only if targeting 1.7.x, which is deferred to Phase 5. Verify master/fork state then. |
| Native AI overrides scripted actions (re-equips, re-targets) | Medium | Design principle: bias, don't puppet. Capability flags + the debug column make contested actions visible instead of mysterious. |
| Per-tick cost across 8 followers | Medium | Staggered scheduling, dependency-driven sensors, instrumented from Phase 1. Measure, don't estimate. |
| No headless test harness exists for Skyrim | Medium | The `RE::`-free architecture line is the entire answer. Pure logic gets real tests; the adapter layer stays thin enough to verify by playing. |
| Follower pathing undermines correct decisions | Low-Medium | Known engine weakness (doorway blocking, terrain snagging). Positioning actions are inherently less reliable than state actions; weight the v1 rule vocabulary toward state, not movement. |
| SKSE plugins cannot hot-reload | Low | Accepted. Rebuild + restart is the loop; fast dev save and `coc` keep it to ~1 min. |

## 6. Open questions to resolve in Phase 1/4

1. Exact C++ API for making an actor consume an `AlchemyItem` — read NPCsUsePotions.
2. Exact field name for the current combat target in `Actor::GetActorRuntimeData()` on
   CommonLibSSE-NG at 1.6.1170 — verify against the live header, AE ABI shuffles have moved
   fields before.
3. Whether `RE::ActorMagicCaster` gives a properly animated forced cast, or whether spell
   control is limited to `EquipSpell` + package steering.
4. The C++ equivalent of `ActorUtil.AddPackageOverride`, and whether package pushes reliably
   become the current package mid-combat (`OnPackageChange` is the observable).
5. Whether to depend on NFF at all. Current answer: **no** — the `kPlayerTeammate` bool bit
   covers NFF's followers without a dependency, and NFF is closed source, last updated
   Apr 2024, with no published API.
6. The exact accessor for BOOL_BITS on the pinned CommonLibSSE-NG version (`boolBits` moved
   behind `GetActorRuntimeData()` in the NG SE/AE split) — one-line check against the header.

---

## 7. Repo layout

```
SkyrimFollowerTactics/
├── docs/            PLAN.md, RESEARCH.md, design notes
├── src/             C++ SKSE plugin
│   ├── core/        Snapshot, Rule, RuleSet, evaluator — NO RE:: types
│   └── game/        registry, scheduler, sensors, actions — all RE:: here
├── tests/           Catch2, runs without Skyrim
├── papyrus/         .psc sources (thin)
├── esp/             the ESL-flagged plugin + CK notes
├── profiles/        shipped default JSON rule sets
└── cmake/, vcpkg.json, CMakeLists.txt
```
