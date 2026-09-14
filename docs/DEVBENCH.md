# devbench: driving the running game

Observations, 2026-09-13. Nothing is wired in and nothing is installed. devbench is alandtse's SKSE plugin that runs a local server inside Skyrim, so a script or an agent can drive and read the game, and other plugins can add tools of their own to it. What follows is read from its source (cloned at `C:\modding\devbench`, v1.18.2, commit `726fa86`) and its Nexus page (mod 181326, updated 2026-09-11). Where a claim rests on the README alone, or is not established, it says so.

## What it is

- **One server, two protocols.** `127.0.0.1:8920` on SE/AE: MCP at `/mcp`, REST at `POST /api/tool/<name>`, one tool registry behind both. `GET /api/health` is answered off the main thread (frame counter, pending tasks), so a busy game reads as busy rather than as a timeout.
- **Should run here; not tried.** It needs SKSE and the Address Library, one build for SE, AE and VR. The installed Address Library carries `versionlib-1-6-1170-0.bin`.
- **Under MO2.** The server writes its port to `Data/SKSE/Plugins/devbench/runtime.json`, which MO2's virtual file system hides from anything MO2 did not launch, so it mirrors the file to `%LOCALAPPDATA%\devbench\se\runtime.json`. `devbench-bridge.exe` is a stdio MCP proxy that survives the game restarting; an MCP client can only spawn it from a real folder, so it has to be extracted out of the archive. The direct route, `claude mcp add --transport http devbench-se http://127.0.0.1:8920/mcp`, drops whenever the game exits.
- **No safety beyond localhost.** No auth, arbitrary console commands. The README says eval-class tools are gated behind an explicit enable; `config.json` documents no such switch and the console handler has no gate.
- **Licence.** The plugin is GPL-3.0-or-later with a modding exception. The two files a consumer compiles, `DevBenchAPI.h` and `DevBenchAPI.cpp`, are MIT and carry no obligation.

## The tools, as they bear on us

| tool | what matters here (read in source) |
|---|---|
| `console` | `exec` queues the command through `AddTask` to `RE::Console::ExecuteCommand` and returns `{queued: true}` without waiting for it to run. `capture: true` fences it between two marker commands; `action: read` returns the console log between the last pair. Console `save`/`load` are rerouted to the `game` tool, because run inside the console they deadlock the engine. |
| `inspect` | `inventory` of the player or any container ref: form, name, type, count, value, weight, equipped, and **no extra-data lists**. `effects` on any actor: spell, effect, magnitude, duration, elapsed. `refs`: one form by id or editor id, the console selection, or the loaded refs by type and radius. Also `player`, `scene`, `mods`, `vm`, `quests`. |
| `papyrus` | `call` runs a global, or a member function on any form named by id (`"self": {"form": "0x000B9986"}`), and returns the result. It waits 3 s for the VM by default; a latent function comes back as a 504. Omitted optional parameters are padded with neutral defaults, so one whose real default is not neutral must be passed. |
| `game` | `list`, `save`, `load`, `loadLast` are fire-and-forget; completion is the `lifecycle` event. `advanceTime` writes the calendar's hour and days directly. |
| `wait` / `sleep` | The engine's own wait, driven to completion before returning; refused where the Wait menu refuses (combat, hostiles near). |
| `menu` | Opens and closes engine menus by name, reads and answers a message box, invokes a handler a mod registered. |
| `scenario` | Steps run server-side, in order: a tool call, a fixed wait, `waitFor` an event, `waitUntil` a state (`playerLoaded`, `noModal`, `noMenu`). `repeat` up to 1000, `continueOnError`; returns a transcript per step. |
| `input` | Keyboard presses under a lease, and VR controllers. No mouse. |
| `capture` | A screenshot. With no provider plugin the fallback is the engine's queued screenshot and a directory poll, which "may include open UI". |
| `record`, `camera` | Record and replay a play-through, a free camera: for benchmarks. |

**`waitFor`, exactly.** Every key in `match` must be present in the payload and equal to it (top-level keys, exact equality). Only events published after the step begins count; the default timeout is 60 s, polled every 100 ms. So an event raised by the previous step, in the moment before the `waitFor` starts, is missed. Our tick's half second makes that unlikely for a rule firing, not impossible.

**Events** are a 256-entry ring in memory, delivered off the publishing thread and read by `GET /api/events?since=N`, server-sent events, or MCP notifications. They are gone when the game exits.

**Its test suite** (`tests/http/`) is a shape worth copying: find the server (an environment variable, `runtime.json`, a port probe), skip the whole session when none answers, bring the game in-world from the main menu if needed, keep scenarios as JSON step lists. `examples/combat_arena.py` spawns three factions and forces a three-way fight with papyrus `MoveTo` and `StartCombat` every tick, and says why: combat AI runs only near the player, and faction hostility alone is unreliable.

## The C ABI

- **Getting it.** `DevBenchAPI::GetDevBenchInterface001()` dispatches an SKSE message to `devbench` and returns null when devbench is absent, so a player without it loses nothing. devbench starts listening in its own `kPostLoad`, which SKSE sends to every plugin before any `kDataLoaded`, so our `OnDataLoaded` (`src/plugin.cpp`) is in time.
- **What it offers.** `RegisterTool(name, descriptorJson, fn, ctx)`: the tool appears on MCP and REST at once, with the descriptor's `description`, `inputSchema` and `readOnly`. `EmitEvent(topic, payloadJson)`. From build 10500, `RegisterToolExtension("inspect" | "menu", key, ...)` adds a kind under a built-in tool.
- **The handler** is a plain C function: the arguments as a JSON string in, the result written once through the host's sink (a result that does not parse comes back as `{raw}`).
- **The handler runs on devbench's listener thread**, not the game's. Anything that touches game state has to `AddTask` once and wait there for the result: never wait from the main thread (it deadlocks), never re-arm (the SKSE gotcha in `CLAUDE.md`).
- **It is game-side code.** `DevBenchAPI.h` includes `RE/Skyrim.h` and `SKSE/SKSE.h`, so it lives in `src/game`.
- **Two ways to consume it.** Copy the two MIT files, or use the overlay port in `cmake/ports/devbench-api`, which pins a commit and its hash. Our `vcpkg-configuration.json` has no `overlay-ports` entry yet.
- **One tool, not an extension.** devbench's README reports cold agents never finding a capability hidden under a built-in tool whose name meant something else. A top-level `followertactics` tool with its actions as an enum is the shape it recommends.

## What it would give us

### The console harness

`docs/TESTING.md` is built around the limits of `bat` files: their lines run in no reliable order, `prid` cannot be batched, a read cannot bracket a write, and a `placeatme` copy has to be clicked. Over devbench:

- A papyrus member call names the follower by ref id and returns the value: `GetActorValue`, `GetItemCount`, `IsInCombat`, `IsDead`. A read after a write is two calls, in order.
- `inspect refs` before and after a `placeatme` finds the new copy, as `combat_arena.py` does.
- `inspect effects` and `inspect inventory` with a `formId` read a follower directly.
- `console exec` still only queues. A read that must follow a console write goes through `papyrus` or `inspect`; whether two consecutive `exec` calls keep their order is not established.
- `cqf DialogueFollower SetFollower <refid>`, unverified in `TESTING.md`, could be tried as a papyrus member call on the quest, with the result returned. Still unverified.

### The in-play backlog

Most of `docs/TODO.md` is "built, not yet run", and each item is a session of hand play today. Those a script could run: tactics in the save (save, load, wait for `postLoadGame`, read the profile events and the state); recruiting from the console; the structured log landing beside the log; pins over a fight and after it; group subjects against a spawned fight; summon and corpse; voice pins. A fight is nondeterministic, so a script asserts on our events and state, never on who wins.

### An agent that reads the game itself

Every measurement in `CLAUDE.md`'s phase notes was read off the log by the person playing. With devbench connected, an agent session can load the save, stage the fight and read the outcome itself.

### Our events on its bus

`docs/LOGGING.md`, "Relationship to devbench", already says how: `ft::log::Emit` also calls `EmitEvent` when the interface is there. A scenario can then block on a real firing: `{"waitFor": {"topic": "followertactics.rule.fired", "match": {"followerId": "0xFF000DE0", "ruleIndex": 0}}}`. The topic needs our prefix, devbench's convention, since the bus cannot tell which plugin emitted. Our form ids already have one spelling, which exact matching needs.

### A tool of our own (a sketch, not a design)

`followertactics`, with actions:

- `state`: each controlled follower's switch, rules and last verdicts, and the snapshot's figures.
- `rules get | set`: the co-save's JSON through `WriteProfile` and `ReadProfile` (`src/core/Profile.h`), so a script writes rules in the format already tested, and `set` answers with `ReadResult`'s warnings.
- `pin | ban | equip`: through the same game-layer functions the panel's click calls, so a script walks the player's path and not a parallel one.
- `bag`: the next section.

devbench cannot click our panel (below), so a tool like this is the only way a script reaches the panel's paths.

### Little, for now

Record and replay, A/B frame timing (the tick's cost is two orders under budget), the camera, VR.

## How it relates to the game model

- **Different layers, not substitutes.** The model (`docs/GAME_MODEL.md`) is the desk loop: seconds, deterministic, and holding only what has been measured. devbench is the engine itself: a minute to boot and load, no CI, an AI that decides differently each run. The eleven bugs of 2026-09-12 would still have needed the game to find; devbench makes that play scriptable, the model makes it fast once found.
- **devbench is how the model's facts would be measured.** Each item of `GAME_MODEL.md`'s "To read or measure before building" (the no-list pick, what the menu splits on, the arrival merge contradicted once, stolen in the menu, the ownership rule, a follower's own loot) is a sequence of give, equip and read, and so are the open items of `docs/UNIQUE.md`'s "To verify in play" that need no disassembly. But `inspect inventory` does not see the lists (ownership, health, poison, the worn marks, text), and the lists are the whole question. The measuring needs our own `bag` action, and its output is exactly Part one's bag view as JSON. The view gets a second consumer, which argues for keeping it plain data with an opaque token, as already planned.
- **Live views as fixtures.** A bag view dumped from the game, checked in as JSON with where it came from (date, build, save, the steps that made it) and loaded by a core test, is a citation that can be run again, where "measured 2026-09-12" cannot. The same goes for a `Snapshot`, which has no JSON form today.
- **Conformance, later.** Once the fake engine exists, one scenario (give, temper, poison, pin, click, hand over) could run twice: under Catch2 against the model and by script against the game, both ending in a bag view, compared. A difference would be a model bug found by machine. Worth it only after both exist; its price is a scenario vocabulary both runners read.

## Found while reading: the tactics clock and `advanceTime`

`TacticsSeconds` (`src/game/Util.cpp`) accumulates game time from the change in the hour of day, wrapped at 24. `game advanceTime` moves the hour and the day count separately, so an advance of 24 hours adds nothing to our clock, 25 hours adds one hour's worth (180 s at timescale 20), and a negative advance of one hour adds 23 hours. A cooldown test should advance less than a day. Whether the same wrap matters in play, for example an engine Wait of 24 hours read by one tick, is not established.

## Limits

- **The panel cannot be clicked.** `input` is the keyboard (and VR), and `menu` opens engine menus by name; SKSE Menu Framework's window is neither. Panel paths are reachable only through a tool we register.
- **Frozen time.** Whether SKSE's task queue drains while our panel freezes time is not established; devbench's main-thread calls give up after 5 s with a 504. Calls are made with the panel closed.
- **Screenshots.** Whether the native `capture` includes the framework's ImGui overlay is unknown; it would decide whether the `docs/*.png` panel shots can be retaken by script.
- **Another hooking plugin.** A console hook, input hooks, a trampoline. It belongs in the development instance only, out of a Nordic Souls reproduction unless meant, and is the first suspect in a crash log.
- **Not a CI check.** It needs the game up. A live suite that skips when no server answers can sit beside the core tests without failing them, as devbench's own does.

## If we take it up

Suggestions in order, each its own decision:

1. No code: install it from Nexus into the MO2 instance, connect Claude Code, and on a save with Marcurio try `inspect effects` and `inspect inventory` on them, `papyrus call Actor.GetActorValue`, and a `game save` then `load`. That answers whether it loads at 1.6.1170 and whether MO2 hides anything.
2. `EmitEvent` from `ft::log::Emit`, as `LOGGING.md` already designs.
3. The `followertactics` tool's `state` and `rules`.
4. A live suite in devbench's style for the in-play backlog.
5. `bag`, with `GAME_MODEL.md`'s first step; the open measurements; the fixtures.
6. Conformance scenarios, after the fake engine.

## Open

- Whether it loads at 1.6.1170 under MO2, and whether the `LOCALAPPDATA` mirror is found.
- Whether two consecutive `console exec` calls run in order.
- Whether SKSE tasks run while a menu freezes time.
- Whether the native `capture` includes the ImGui overlay.
- Whether `DialogueFollower`'s `SetFollower`, called through `papyrus`, fills the alias.
