# Logging

Two channels, four levels, and one call that writes both. Built 2026-09-09; not yet verified in play.

Before it, every `logger::` call in `src/game` wrote prose at `info` (the level was hardcoded in `plugin.cpp`, so nothing was ever logged at `debug` — nothing would have shown), with the follower's name as a free interpolated `{}` and the module as a string prefix somebody typed (`"tactics: "`, `"packages: "`, `"pins: "`, and on most lines nothing at all). That was fine to read live and bad to query: no field to `grep` on, and a one-time byte-level calibration probe sat in the same bucket as "FIRED rule 0 -> performed."

## Two channels

- **`FollowerTactics.log`** — prose, for a human tailing it while playing. Still the thing every doc's play-test evidence quotes.
- **`FollowerTactics.events.jsonl`** — beside it. One JSON object per line, for the events worth querying after the fact: state changes, not narration. Not every call site is here — the calibration dumps in `Packages.cpp` and the per-tick sensor dumps in `Sensors.cpp` have no query value in either format and are prose-only, at `debug`.

Both come out of the same `event()` call at the point the event happens; nobody hand-maintains two logs of the same fact.

## The module is a field

Every prose line names its module, and the module comes from the `ft::log::Module` the call went through rather than from a string at the front of the message:

```
[14:02:11.4] [info] [tactics]   Lydia (000A2C94) FIRED rule 0 "emergency heal" [drink-strongest] -> performed
[14:02:11.4] [info] [packages]  slot 3 aims at 000A2C94 "Lydia"
[14:02:11.4] [warn] [pins]      Lydia (000A2C94) the engine would equip Iron Sword over pinned Dagger -- refused
```

so `grep '\[packages\]'` works, and the messages line up in a column whatever the module. There is no `logger` alias any more (`src/PCH.h` says why): `ft::log::<module>.info(...)` is the only way in, and a new call site cannot quietly go back to prose at a hardcoded level with a hand-typed prefix.

The ten modules are one per source file that logs: `actions`, `forms`, `hits`, `packages`, `pins`, `plugin`, `profiles`, `sensors`, `tactics`, `ui`.

## Levels

Set in `Data/SKSE/Plugins/FollowerTactics.ini`, read once at load:

```ini
[Log]
level  = info      ; error | warn | info | debug
events = true      ; the .jsonl sidecar
```

Every value in the shipped file is its default, so deleting the file changes nothing. The level filters **both** channels: `level = warn` means warnings and errors in the prose log and in the sidecar alike.

| level | rule | examples |
|---|---|---|
| `error` | a feature is broken until something changes | a package, word or shout that could not be made; the `EquipObject` detour failing to install; a probe that turns cast rules off; a co-save record that could not be written |
| `warn` | the mod adapted or skipped something on its own | an action that did not take effect; the pool exhausted at dispatch; an item above the follower's skill refused a pin; a saved rule, pin or ban dropped; saved tactics that could not be read |
| `info` | a state change worth narrating while playing | entering and leaving a fight; a rule firing; a pin or ban applied, released, restored or refused; a package armed, fired or released; an item consumed; the profile loaded and saved |
| `debug` | everything else | per-tick health readouts and per-rule verdicts; inventory and active-effect dumps; the `Packages.cpp` calibration probes; the AI score hook's answers; readbacks after an equip |

Two reclassifications the levels forced, both of which had been hiding something:

- `Profiles.cpp`, "the saved tactics could not be read — starting with none", was `error` and is `warn`. It recovers cleanly to an empty rule list, which is the `warn` definition.
- Every probe line in `Packages.cpp` ending "cast rules stay off" was `info` and is now `error` with a `pool.unavailable` event. The feature is off until something changes; reporting that at `info` beside a hex dump is how it could turn itself off unnoticed.

## The envelope

Every `.jsonl` line carries the same five header fields, then the follower it is about, then the event's own:

```json
{"ts":"2026-09-09T14:02:11.400Z","level":"info","plugin":"FollowerTactics","version":"0.1.0",
 "event":"rule.fired","followerId":"0xFF000DE0","followerName":"Lydia",
 "ruleIndex":0,"ruleName":"emergency heal","action":"drink-strongest",
 "targetFormId":"0xFF000DE0","outcome":"performed","healthPct":0.49}
```

`ts` is UTC with milliseconds, so lines sort across a DST boundary and across machines. `version` is `CMakeLists.txt`'s `project(... VERSION)`, so a `.jsonl` attached to a bug report says which build wrote it. Every form id is a string in one spelling — `0x` and eight upper-case digits — so a query keys on one form of it; `followerId`/`followerName` follow `PROFILES.md`'s convention, the id to key on and the name for the human reading the output, never read back.

There is deliberately no log-schema-version field. That problem belongs to the co-save (`PROFILES.md`'s `schema` key), which has to keep reading old saves forever; a log line has no such lifetime.

## Events

`error` and `warn` events carry that level; the rest are `info` unless noted.

| event | module | beyond the envelope |
|---|---|---|
| `plugin.loaded` | plugin | (none: the packages, the tick, the panel and the hooks are up) |
| `tactics.installed` | tactics | `tickMs`, `maxFollowers` |
| `tactics.switched` | tactics | `enabled` |
| `tactics.cost` | tactics | `evaluations`, `avgUs`, `maxUs` |
| `followers.controlled` | tactics | `count`, `followers[]` |
| `combat.entered` / `combat.left` | tactics | `allies[]`, `enemies[]` (entered only) |
| `follower.down` / `follower.up` | tactics | — |
| `rule.fired` | tactics | `ruleIndex`, `ruleName`, `action`, `targetFormId`, `outcome`, `healthPct` |
| `rule.actionFailed` | tactics | `ruleIndex`, `ruleName`, `action`, `reason` — **warn** |
| `poison.applied` | actions | `poisonFormId`, `poisonName`, `weaponFormId`, `weaponName` |
| `soul.spent` | actions | `gemFormId`, `soul`, `weaponFormId`, `chargeBefore`, `chargeAfter`, `chargeMax` |
| `pin.applied` / `pin.released` / `pin.restored` | pins | `itemFormId`, `itemName`, `hand`, `reason` |
| `pin.refused` | pins | `itemFormId`, `hand`, `reason`, and `refusedFormId` when the engine's own equip was blocked — **warn** |
| `ban.applied` / `ban.released` / `ban.enforced` | pins | `itemFormId`, `itemName` |
| `ban.refused` | pins | `itemFormId`, `inCombat` — **warn** |
| `equip.applied` / `equip.removed` | pins | `itemFormId`, `hand`, `pinned` |
| `unequip.applied` | pins | `itemFormId`, `itemName`, `hand` — the player's page, which touches no pin or ban |
| `dualWield.allowed` | pins | `styleFormId`, `copyFormId` |
| `package.armed` | packages | `slot`, `formId`, `holderFormId`, `kind`, `targetFormId`, `durationS` |
| `package.fired` | packages | `slot`, `formId`, `holderFormId`, `kind` |
| `package.released` | packages | `slot`, `holderFormId`, `durationS`, `reason` |
| `pool.ready` | packages | `spellSlots`, `voiceSlots` |
| `pool.exhausted` | packages | `kind`, `slots` — **warn** |
| `pool.unavailable` / `pool.slotFailed` | packages | `reason`, `slot` — **error** |
| `scroll.spent` | packages | `formId`, `by`, `carriedBefore`, `carriedAfter` |
| `profile.loaded` / `profile.saved` / `profile.claimed` | profiles | `recordCount`, `ruleCount`, `pinCount`, `enabled` |
| `profile.entryDropped` | profiles, pins | `kind` (`rule`/`pin`/`ban`/`record`), `label`, `reason` — **warn** |
| `profile.newerSchema` | profiles | `key`, `schema`, `known` — **warn** |
| `profile.saveFailed` | profiles | `key`, `reason` — **error** |
| `form.error` | forms | `requestedFormId`, `kind`, `reason` — **error** |
| `install.failed` | pins, hits, profiles | `what`, `reason` — **error**, except the hit sink, which degrades to "Attacked by is never true" |
| `ui.installed` / `ui.unavailable` | ui | `reason` |

Diagnostic-only call sites do not get an event name; they are prose-only at `debug`. Two events are the exception that proves it: `package.otherCast` and `package.otherVoice`, at `debug`, are the same line as `package.fired` for a cast that turned out to be the follower's own — a `.jsonl` at `debug` can then tell "ours fired" from "something fired" without parsing prose.

## Where the code is

`src/core/LogEvent.{h,cpp}` is the envelope: levels, fields, ids, and one line of JSON. It needs no game, so it has tests (`tests/test_logevent.cpp`) — the envelope's shape, how each kind of value is spelled, and that a rule label the player typed with a quote in it cannot produce an unparseable line.

`src/game/Log.{h,cpp}` is everything that needs the game or the disk: the two sinks, the ini, the clock, the actor's name, and the `Module` objects. The split is the architectural rule in `CLAUDE.md`, and it is what lets the line format be pinned by a test rather than by reading a log.

## Why not JSON for everything

The project's own play-test workflow is tailing `FollowerTactics.log` and reading it as it scrolls — every measured claim in `CLAUDE.md`'s phase writeups is a human reading prose. A `.jsonl` is worse for that and better for the other half: "did they cast twice", a before/after across two runs, or a file attached to a bug report that needs no tooling to read. Splitting the two keeps each doing the half it is good at, instead of one doing both badly.

## Relationship to devbench

If `devbench` is ever wired in via the C-ABI, its `EventBus` is a 256-entry **in-memory ring with no disk persistence** — it exists only while the game is up and only for whoever is polling `/api/events`. The `.jsonl` sidecar is the opposite: persistent, needs no server, survives the process exiting. They are two transports for the same fact, so `ft::log::Emit` is the single point both would come from: always append the JSON line, and if `DevBenchAPI::GetDevBenchInterface001()` is non-null, also `EmitEvent()` the identical payload. Nothing here depends on devbench existing.
