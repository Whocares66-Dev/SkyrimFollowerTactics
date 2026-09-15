# Logging

Two channels, four levels, and one call that writes both. Built 2026-09-09; not yet verified in play.

**Since 2026-09-14 the events file holds game events only**, written whatever the level, and a game event is also kept in memory and archived with its session. `docs/EVENTS.md` is which events there are, what each carries, and how the files are kept; this is the machinery: the two channels, the levels, the envelope, and how to add a call site.

Before it, every `logger::` call in `src/game` wrote prose at `info` (the level was hardcoded in `plugin.cpp`, so nothing was ever logged at `debug` — nothing would have shown), with the follower's name as a free interpolated `{}` and the module as a string prefix somebody typed (`"tactics: "`, `"packages: "`, `"pins: "`, and on most lines nothing at all). That was fine to read live and bad to query: no field to `grep` on, and a one-time byte-level calibration probe sat in the same bucket as "FIRED rule 0 -> performed."

## Two channels

- **`FollowerTactics.log`** — prose, for a human tailing it while playing. Still the thing every doc's play-test evidence quotes.
- **`FollowerTactics.events.jsonl`** — beside it. One JSON object per line: the game events (`docs/EVENTS.md`), what tactics did to and saw of a follower, whatever the level. Everything else — a follower's cast records, forms, hooks, the profile, the panel, the tick's cost, the calibration and sensor dumps — is prose only.

Both come out of the same `event()` call at the point the event happens; nobody hand-maintains two logs of the same fact.

## The module is a field

Every prose line names its module, and the module comes from the `ft::log::Module` the call went through rather than from a string at the front of the message:

```
[14:02:11.4] [info] [tactics]   Lydia (000A2C94) FIRED rule 0 "emergency heal" [drink-strongest] -> performed
[14:02:11.4] [info] [packages]  FF3F0800 aims at 000A2C94 "Lydia"
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

Deleting the file gives the defaults: `info`, and the events file on. The level filters the prose log only: `level = warn` means warnings and errors in `FollowerTactics.log`, and every game event still in the events file.

| level | rule | examples |
|---|---|---|
| `error` | a feature is broken until something changes | a package, word or shout that could not be made; the `EquipObject` detour failing to install; a probe that turns cast rules off; a co-save record that could not be written |
| `warn` | the mod adapted or skipped something on its own | an action that did not take effect; an item above the follower's skill refused a pin; a saved rule, pin or ban dropped; saved tactics that could not be read |
| `info` | a state change worth narrating while playing | entering and leaving a fight; a rule firing; a pin or ban applied, released, restored or refused; a package armed, fired or released; an item consumed; the profile loaded and saved |
| `debug` | everything else | per-tick health readouts and per-rule verdicts; inventory and active-effect dumps; the `Packages.cpp` calibration probes; the AI score hook's answers; readbacks after an equip |

Two reclassifications the levels forced, both of which had been hiding something:

- `Profiles.cpp`, "the saved tactics could not be read — starting with none", was `error` and is `warn`. It recovers cleanly to an empty rule list, which is the `warn` definition.
- Every probe line in `Packages.cpp` ending "cast rules stay off" was `info` and is now `error`. The feature is off until something changes; reporting that at `info` beside a hex dump is how it could turn itself off unnoticed.

## The envelope

Every `.jsonl` line carries the same five header fields, then the follower it is about, then the event's own:

```json
{"ts":"2026-09-14T19:53:11.400Z","level":"info","plugin":"FollowerTactics","version":"0.1.0",
 "event":"rule.fired","followerId":"0xFF000DE0","followerName":"Lydia",
 "ruleIndex":0,"ruleName":"emergency heal","subjectKind":"self",
 "subjectFormId":"0xFF000DE0","subjectBaseFormId":"0x000A2C94","subjectName":"Lydia",
 "action":"drink-strongest","targetFormId":"0xFF000DE0","targetBaseFormId":"0x000A2C94","targetName":"Lydia",
 "outcome":"performed","healthPct":0.49}
```

`ts` is UTC with milliseconds, so lines sort across a DST boundary and across machines. `version` is `CMakeLists.txt`'s `project(... VERSION)`, so a `.jsonl` attached to a bug report says which build wrote it. Every form id is a string in one spelling — `0x` and eight upper-case digits — so a query keys on one form of it; `followerId`/`followerName` follow `PROFILES.md`'s convention, the id to key on and the name for the human reading the output, never read back. Every number is rounded to three decimals on its way in, so a duration reads in milliseconds and a fraction to a tenth of a percent, rather than carrying the noise a float brings with it when it widens to a double.

There is deliberately no log-schema-version field. That problem belongs to the co-save (`PROFILES.md`'s `schema` key), which has to keep reading old saves forever; a log line has no such lifetime.

## Events

Which events there are, what each carries, and when they are written is `docs/EVENTS.md`. A game event carries its level in the envelope -- `warn` for `rule.actionFailed` and `session.ceiling`, `debug` for `rule.verdict`, `info` for the rest -- and the level decides its prose line and nothing about the events file.

A diagnostic has no event name: it is a prose line, at the level the table above gives it. A cast of ours leaving a hand is `info` and the follower's own is `debug`, one line through `Module::at`.

## Where the code is

`src/core/LogEvent.{h,cpp}` is the envelope: levels, fields, ids, and one line of JSON. It needs no game, so it has tests (`tests/test_logevent.cpp`) — the envelope's shape, how each kind of value is spelled, and that a rule label the player typed with a quote in it cannot produce an unparseable line.

`src/game/Log.{h,cpp}` is everything that needs the game or the disk: the two sinks, the ini, the clock, the actor's name, and the `Module` objects. The split is the architectural rule in `CLAUDE.md`, and it is what lets the line format be pinned by a test rather than by reading a log.

## Why not JSON for everything

The project's own play-test workflow is tailing `FollowerTactics.log` and reading it as it scrolls — every measured claim in `CLAUDE.md`'s phase writeups is a human reading prose. A `.jsonl` is worse for that and better for the other half: "did they cast twice", a before/after across two runs, or a file attached to a bug report that needs no tooling to read. Splitting the two keeps each doing the half it is good at, instead of one doing both badly.

## Relationship to devbench

If `devbench` is ever wired in via the C-ABI, its `EventBus` is a 256-entry **in-memory ring with no disk persistence** — it exists only while the game is up and only for whoever is polling `/api/events`. The `.jsonl` sidecar is the opposite: persistent, needs no server, survives the process exiting. They are two transports for the same fact, so `ft::log::Emit` is the single point both would come from: always append the JSON line, and if `DevBenchAPI::GetDevBenchInterface001()` is non-null, also `EmitEvent()` the identical payload. Nothing here depends on devbench existing.
