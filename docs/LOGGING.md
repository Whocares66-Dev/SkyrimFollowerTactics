# Logging

Today every `logger::` call in `src/game` writes prose at `info` (the level is hardcoded in `plugin.cpp`; nothing is ever logged at `debug` because nothing would show), with the follower's name as a free interpolated `{}` and the module as a string prefix (`"tactics: "`, `"packages: "`, `"pins: "`). That is fine to read live and bad to query: there is no field to `grep` on, and a one-time startup calibration probe sits in the same bucket as "FIRED rule 0 -> performed." This is a design for two channels that serve different needs, not a rewrite of one into the other.

## Two channels

- **`FollowerTactics.log`** — prose, for a human tailing it while playing. Stays as it is: free text, whatever level is configured, the thing every existing doc's play-test evidence already quotes.
- **`FollowerTactics.events.jsonl`** — new, beside it. One JSON object per line, written only for the events worth querying after the fact: state changes, not narration. Not every call site moves here — the byte-offset calibration dumps in `Packages.cpp` and the per-tick sensor dumps in `Sensors.cpp` have no query value in either format and stay prose-only, at `debug`.

Both are written by the same emit call at the point the event happens; nobody hand-maintains two logs of the same fact.

## Levels

The level is currently hardcoded (`plugin.cpp:31`, `spdlog::level::info`); making it configurable is a prerequisite for `debug` to mean anything. The four levels, by what actually belongs in each:

| level | rule | examples |
|---|---|---|
| `error` | a feature is broken until something changes | `Forms.cpp`: package/shout creation failed; `Pins.cpp`: the `EquipObject` detour failed to install; a co-save record fails to parse |
| `warn` | the mod adapted or skipped something on its own; worth knowing, not broken | "action did NOT take effect"; "pool exhausted at dispatch"; "cannot be pinned: above the follower's skill"; a dropped rule/pin/ban per `PROFILES.md`'s versioning section |
| `info` | a state change worth narrating while playing | entered/left combat; rule fired; pin/ban applied or released; package armed/fired/released; profile loaded/saved (counts only) |
| `debug` | everything else presently cluttering `info` | per-tick health readouts outside a firing event; per-hand inventory and active-effect dumps; the `Packages.cpp` calibration probes; off by default |

One reclassification this surfaces: `Profiles.cpp:193` ("the saved tactics could not be read — starting with none") is logged at `error` today but recovers cleanly to an empty rule list, which is the `warn` definition above, not `error`.

## The envelope

Every `.jsonl` line carries the same header fields, then event-specific ones:

```json
{ "ts": "2026-09-09T14:02:11.4Z", "level": "info", "plugin": "FollowerTactics", "version": "0.1.0",
  "event": "rule.fired", "followerId": "0xFF000DE0", "followerName": "Lydia",
  "ruleIndex": 0, "ruleName": "emergency heal", "action": "drink-strongest",
  "healthPct": 0.49 }
```

`version` is `CMakeLists.txt`'s `project(... VERSION)`. No log-schema-version field: that problem belongs to the co-save (`PROFILES.md`'s `schema` key), which has to keep reading old saves forever; a log line has no such lifetime and versioning it would be solving a problem that doesn't exist yet. `followerId`/`followerName` follow `PROFILES.md`'s own convention: the id is what a query keys on, the name is for a human reading the query's output, never read back.

## Events

| event | level | fields beyond the envelope |
|---|---|---|
| `combat.entered` / `combat.left` | `info` | `allies[]`, `enemies[]` (formIds) |
| `rule.fired` | `info` | `ruleIndex`, `ruleName`, `action`, `targetFormId`, `healthPct` |
| `rule.actionFailed` | `warn` | `ruleIndex`, `ruleName`, `action`, `reason` |
| `pin.applied` / `pin.released` | `info` | `itemFormId`, `hand`, `reason` |
| `pin.refused` | `warn` | `itemFormId`, `refusedFormId` — the engine's own equip that was blocked |
| `ban.applied` / `ban.released` | `info` | `itemFormId` |
| `package.armed` / `package.fired` / `package.released` | `info` | `slot`, `formId` (spell or shout), `holderFormId`, `durationS`, `reason` (released only) |
| `profile.loaded` / `profile.saved` | `info` | `recordCount`, `ruleCount`, `outcome` |
| `profile.entryDropped` | `warn` | `kind` (`rule`/`pin`/`ban`), `ruleIndex` (rules only), `label`, `reason` — the `PROFILES.md` "every drop is one warn line" behavior, structured |
| `form.error` | `error` | `requestedFormId`, `kind` (spell/shout/package), `reason` |

Diagnostic-only call sites (calibration probes, per-tick sensor/inventory dumps) do not get an `event` name; they stay prose-only at `debug`.

## Why not JSON for everything

The project's own play-test workflow is tailing `FollowerTactics.log` live and reading it as it scrolls — every measured claim in `CLAUDE.md`'s phase writeups is a human reading prose, not a script reading JSON. A `.jsonl` file is worse for that and better for the other half of the job: "did she cast twice," a before/after comparison across two runs, or attaching a file to a bug report without anyone needing to install anything. Splitting the two keeps each format doing the half it's actually good at, instead of one format doing both badly.

## Relationship to devbench

If `devbench` (see its own investigation) is ever wired in via the C-ABI, its `EventBus` is a 256-entry **in-memory ring with no disk persistence** — it exists only while the game process is up and only for whoever is polling `/api/events` or subscribed live. The `.jsonl` sidecar is the opposite: persistent, needs no server or agent attached, survives the process exiting. They are two transports for the same fact, not competing designs, so the emit call this doc describes should be the single point both come from: always append the JSON line; if `DevBenchAPI::GetDevBenchInterface001()` is non-null, also `EmitEvent()` the identical payload onto its bus. Nothing here depends on devbench existing.
