# Game events

What tactics record about the game: which rules were weighed and why they did or did not act, whom a rule was about and whom it aimed at, what came of a cast, and what happened to a follower's pins and bans. Built 2026-09-14; **not yet verified in play** (the list at the end). `docs/LOGGING.md` is the machinery underneath (the prose log, the levels, the JSON envelope); this is what goes through it as a game event, where the events are kept, and how the files are managed.

## Why

The rule table had a Status column: one word per rule for its last evaluation. Evaluation runs only in a fight, twice a second; the column went blank the tick after a fight ended, and "fired" lasted one tick, so it was rarely seen. It is gone, and the same knowledge goes here instead, where it lasts: in the events file for reading afterwards, and in memory for a Logs tab to show as it happens.

Before this, the events file mixed game events with the mod's plumbing (the package pool, forms, the profile, the tick's cost); `rule.fired` said "performed" for a cast that had only been requested and named no subject and no item; nothing tied a cast's real outcome to its rule; the watchdog reported the same put-back every half second while an equip failed to take; and two pin reasons blamed the player for what a rule had done.

## Where an event goes

A game event is one `Module::event()` call, and it goes to three places:

- **`FollowerTactics.log`**, as a prose line, when its level passes the ini's `level`. The prose log is for reading while playing, and carries the diagnostics too.
- **`FollowerTactics.events.jsonl`**, as a JSON line, whenever `events = true`, **whatever `level` says**. Turning the prose log down to warnings must not erase the record of a fight.
- **Memory**, always: the last 1,000 events, for the panel.

Everything that is not a game event -- the plugin loading, a follower's cast records and the forms they are made of, hooks installing, the profile's save and load, the panel, the tick's cost -- is prose in `FollowerTactics.log` only.

## The panel reads memory, not the file

SKSE Menu Framework can leave time running while the panel is open, and the tick already handles both settings, so a Logs tab has to tail: show an event the moment it happens. Reading it from the file would mean file I/O and JSON parsing for the render thread, a half-written last line, a file up to a second behind its buffer, a file that may be switched off, and one that is renamed at the next launch. Reading it from memory is a lock, a comparison of the newest sequence number with the last one seen, and a copy of what is new, with the fields already parsed. The file is for reading a session afterwards.

## The last 1,000 in memory

- A bounded ring, `ft::EventRing`, in `src/core` (no game types, tested): capacity 1,000, the oldest dropped first.
- Each entry: a sequence number, the timestamp, the level, the event name, the follower's id and name, the prose line, and the fields. The sequence number is never reused, so a reader asks for what is newer than the last number it saw, however many were dropped in between.
- The game side keeps one ring behind a mutex, pushed from `Emit` on whatever thread emitted. The panel copies what is new under the lock, as it copies the follower views, and never holds the lock while drawing.
- What an entry holds, it owns: the names and the prose are strings, a field's value is its own; a field's key is a string literal, static for the life of the process, which is the rule `Field` already relies on.
- An entry is under a kilobyte, so the ring stays under about a megabyte.
- It fills whether or not the events file is on, and whatever the level.
- A load does not clear it: what happened before a reload is what someone wants to read. A `game.loaded` event marks the break.
- The 1,000 are shared by every follower. Eight followers in a long fight could push one follower's early events out; how long 1,000 lasts is to be counted in play before anything finer is built.

## Sessions and files

**A session is one launch of the game.** The plugin loads once per process, which is the one moment that is unambiguously the start of a sitting. Loading a save inside a launch stays in the same files, marked by `game.loaded`: a file per load would scatter one sitting and part a death from the reload that followed it.

**The current session keeps the fixed names**, `FollowerTactics.log` and `FollowerTactics.events.jsonl`, in the SKSE log folder, so tailing them and every doc that names them keep working.

**The previous session is archived, not overwritten.** Until now both files were opened with truncation at every launch: they never outgrew a session, but the session before was lost, including the log behind a crash, since Crash Logger's `crash-<UTC>.log` survives the relaunch and ours did not. At launch, before the new files open, the old pair moves to the subfolder `SKSE/FollowerTactics/` -- the SKSE folder is shared with every other plugin's log and the crash logs -- under one stem:

```
FollowerTactics-2026-09-14-19-53-02_2026-09-14-20-10-03.log
FollowerTactics-2026-09-14-19-53-02_2026-09-14-20-10-03.events.jsonl
```

The timestamps are the session's start and end in UTC, in Crash Logger's own format, so an archived session's name brackets any crash that fell inside it, and the names sort by time.

**Start and end.** The start is written by the session itself: the events file's first line is a `session.started` event, and the prose log's first line a banner with the UTC date and time (the prose line pattern carries only the time of day). At the next launch the start is read back from the events file's first line, else the banner. The end is the later of the two files' last-write times. The files' creation times cannot serve: truncation keeps a file's creation time (on 2026-09-14 the log read created 9/9 and was rewritten that day), and NTFS gives a file created under a name just renamed away the old file's creation time. A pair whose start cannot be read -- files from before this, an empty file -- is archived by its end alone, `FollowerTactics-_<end>`.

**Retention.** The archive keeps the 20 most recent sessions and deletes older pairs, oldest name first. One session on 2026-09-14 wrote 151 KB of prose and 29.5 KB of events, so twenty is a few megabytes.

**A ceiling per file per session**, against a runaway session, the debug level left on for hours: 64 MB. Past it, one line says the ceiling was reached and nothing more is written to that file this session; the other file and the ring carry on. The bytes are counted as they are handed to the logger.

**When the move fails** -- another program has the file open without sharing deletion, an editor say -- the file is copied into the archive and truncated as before, and the new session's log says so.

**Buffering.** spdlog's file sink (1.16, `details/file_helper-inl.h`) opens the file with `fopen` and writes with `fwrite`, so the C runtime already buffers; flushing after every line, as both files did, defeated it. The events file flushes on a warning or an error, and once a second otherwise, by `spdlog::flush_every`. That flusher reaches only loggers registered with spdlog (`registry::flush_all`), so the events logger is registered; the prose logger already is, as the default logger. A crash loses at most a second of events; the prose log still flushes every line, because the last line before a crash is the one that matters most, and it carries the same events as prose. No asynchronous logger and no batching of our own: the rate is a few to a few dozen lines a second, and the `_mt` sink already serialises the writers and the flusher. Fewer, larger writes also mean fewer change notifications for OneDrive, which holds Documents on this machine (not measured).

**What is tested without the game:** reading a session's start from either first line, the archive stem with and without a start, and which archived stems to delete for a count to keep. Moving, copying and reading file times are the game side's, through `std::filesystem`.

## Who and what an event names

- **Every event about a follower** carries `followerId` and `followerName` first, as the envelope always has.
- **An actor** a rule was about or aimed at: the reference FormID, the base FormID, and the display name. The base is `Actor::GetTemplateBase()`, the leveled template's base where there is one, else `GetActorBase()`; which of the two matches the record in the plugin is to be checked on a leveled bandit against xEdit. Both are captured when the rule acts, since the actor may be unloaded by the time an outcome arrives.
- **A list of actors** -- `allies`, `enemies`, `followers` -- is an array of objects, each with `formId`, `baseFormId` and `name`: the three an actor field carries, so an actor reads alike wherever an event names one.
- **An item or a spell**: the base FormID and the name, and for an item the variant as one string -- `any`, `plain`, or for example `tempered 1.20; enchanted 0x0001A2B3@25/60/0; named "Frost Fang"` -- from `VariantText` in core. An item in a bag has no reference and no FormID of its own; a copy is told from another by its variant (`docs/UNIQUE.md`).
- **The item in a pin or ban event** is `itemFormId` and `itemName`, with `hand` -- `[L]`, `[R]`, `[LR]`, or empty for armour, ammunition, the voice and a ban -- and `variant`.
- **A field's key is a string literal.** A helper that appends an actor's three fields takes its three keys as arguments, `AppendActor(out, "subjectFormId", "subjectBaseFormId", "subjectName", id)`; a key built at run time would dangle.

## The catalogue

| event | when | beyond the follower |
|---|---|---|
| `session.started` | the first line of every session | — (its `ts` is the session's start) |
| `session.ceiling` (warn) | the events file has taken its 64 MB for the session; nothing more is written to it | `megabytes` |
| `game.loaded` | a save loaded or a new game begun | `newGame` |
| `game.saved` | the game is about to write a save; a cast the save cuts short is resolved just before it, reason `saving` | `saveName` |
| `combat.entered` / `combat.left` | the follower's fight begins or ends | `allies[]`, `enemies[]` (entered; each an actor) |
| `follower.down` / `follower.up` | bleeding out, and up again | — |
| `followers.controlled` | who is under tactics changes | `count`, `followers[]` (each an actor) |
| `tactics.switched` | the master switch | `enabled` |
| `rule.fired` | a rule's action is dispatched | `ruleIndex`, `ruleName` (the rule as it began), `subjectKind` and the subject's ids, `action`, the target's ids, the form and variant, `outcome`, `followerHealthPct` (the follower's own health, whoever the subject) |
| `rule.actionFailed` (warn) | the outcome is neither `performed` nor `requested`, a refused pin included | `ruleIndex`, `ruleName`, `action`, `reason` |
| `rule.resolved` | a requested cast, scroll, shout, power, power attack or bash is over | `ruleIndex`, `ruleName`, `kind`, `outcome` (`cast` or `not-cast`; `made` or `not-made` for a power attack or a bash), `reason`, `durationS`. A cast, scroll, shout, power or power attack adds the form (a power attack's is the weapon in the right hand), the target's ids, `pickedUp` and `placedIn` (`alias packages`, the kind of override list the record was put at the front of, or `nowhere`), and a power attack its `attackEvent`, `staminaCost` and `reach` as priced, the follower's `staminaAtRequest` and `staminaAtEnd`, `distanceAtRequest` and `distanceAtEnd` to the target (as the engine's melee test measures it: centre to centre, less both bodies), `headingAtRequest` and `headingAtEnd` (degrees between the follower's facing and the target), and `attackStateAtEnd` (the engine's attack state; -1 where there is no one to measure). A bash or power bash adds `alreadyBlocking`, `blockRaised`, `blockUpS` and `sentS` (seconds after the request, -1 for never), `blockRefusals` and `bashRefusals` (`docs/ATTACK.md`) |
| `rule.verdict` | a rule's verdict differs from the last one reported | `ruleIndex`, `ruleName`, `verdict`, `reason` |
| `poison.applied` | a poison on a blade | the poison, the weapon |
| `soul.spent` | a soul gem into a weapon | the gem, the soul, the weapon, the charge before, after and at most |
| `scroll.spent` | a scroll read | the scroll, `by`, carried before and after |
| `equip.applied` | the panel readies a thing without pinning it | the item, `hand`, the variant |
| `pin.applied` / `pin.released` | a pin made or let go | the item, `hand`, the variant, `by`, `reason` |
| `pin.overridden` | a rule's pin, or its None, in a fight displaces a pin the follower had before the fight | the displaced item, `hand`, the variant, `by: rule`, `overriddenByFormId` and `overriddenByName` (`0x00000000` and "nothing: the AI decides" for a None) |
| `ban.overridden` | a rule pins a banned item | the item, `hand`, the variant, `by: rule`, `inCombat` |
| `pin.restored` | the fight is over and a pin from before it is pinned again, and put back on | the item, `hand`, the variant, `by: fight-end` |
| `pin.enforced` | a pinned thing was found off and is put back: once per violation | the item, `hand`, the variant, `displacedByFormId` and `displacedByName` (what the voice or the pinned hand held instead; nothing for armour), `reason` |
| `ban.enforced` | a banned thing was found on and is taken off: once per violation | the item, the variant, the `hand` it was in, and `by: fight-end` when a rule's pin on it has just gone with the fight |
| `ban.applied` / `ban.released` | a ban made or lifted | the item, the variant, `by` |

**`by`** says who did it: `player` (the panel), `rule`, `fight-end` (the after-fight restore), `save` (taken back from the save), `game` (the thing is no longer carried).

**`outcome`** on `rule.fired`: `performed` for what is done at once -- a drink, a poison, a soul gem, an equip, a power attack sent as an event -- and `requested` for a cast, scroll, shout, power, power attack or bash, whose own outcome follows in `rule.resolved`; anything else is the failure, and `rule.actionFailed` repeats it as a warning.

**`rule.resolved` pairs with the follower's last `rule.fired` whose outcome was `requested`**: a follower has at most one request in flight -- a cast, a power attack or a bash -- and no rule of theirs can make another meanwhile. It carries the rule's index and name as well, so it reads alone.

**`reason`** on `rule.resolved` is why the request ended. A cast's, scroll's, shout's or power's is the package's own release reason: `spell fired`, `shout fired`, `power fired`, `stream ended`, `target dead`, `package ended`, `deadline, never cast`, `deadline, AI never picked it up`, `deadline, stream still running`, `holder vanished`, `saving`. A power attack's: `power attack made`, `package ended`, `package ended mid-swing`, `deadline, AI never picked it up`, `deadline, no power attack`, `deadline, still swinging`, `holder vanished`, `saving`. A bash's: `bash made`, `taken, never bashed`, `watch over, still bashing`, `deadline, weapon never drawn`, `deadline, still mid-swing`, `deadline, block refused`, `deadline, block never up`, `deadline, bash refused from the block`, `holder vanished`, `no actor state`, `saving`.

**`rule.verdict`** carries the verdict's wire name (`no-resource`, `cooldown`, `condition-false`, ...) and the sentence the Status column's tooltip used to show. Its prose line is at `debug`, so the prose log stays quiet at `info`; the events file has every one.

### Not game events

Prose in `FollowerTactics.log` only: the plugin loading; the tick installing and its cost; a follower's cast records being made, and a package armed, fired or released; the forms they are made of; a hook that failed to install; the profile's save, load and dropped entries; the panel installing; the player's own page putting something away; and a pin or ban **refused**, whether by the equip detour stopping the engine before it broke one or by a spell above the follower's skill. A refusal changes nothing on the follower; a rule's refused pin is reported by `rule.actionFailed`.

## How the harder ones are known

**The rule as it began, and whom its condition bound.** A rule with several actions does one a tick, and the rules can be edited while it is part way through. The list in progress carries a copy of the rule it began with and the actor its condition bound, beside the target it already carried, so `rule.fired` names the rule that is acting even after a reorder or a delete (it looked the name up by index in the current list, and named the wrong rule), and names its subject. The decision carries the same. The subject is 0 for Corpse: None, whose binding is the follower themself and would read as a corpse.

**A verdict reported when it changes.** Each follower keeps the verdict last reported for each rule. After an evaluation, a rule whose verdict is different is reported, with three exceptions: "not reached" says nothing and keeps the last value; "fired" updates it silently, since `rule.fired` says so; and the farewell evaluation after a fight is skipped, since every standing rule turns false on it. The record starts again when a fight begins and when the rules change. Which rules to report is a pure function in core, tested; the verdict's wire name sits beside `ToString` in `Evaluator.cpp`.

**A cast's outcome.** A cast, scroll, shout or power is a request: a package on the follower's stack, released later. When the request is armed, the rule's index and name are written onto the follower's cast record, and where the record is released -- the tick's release, a holder gone, a save -- `rule.resolved` is emitted with whether the spell was cast, whether the AI picked the package up, and the release reason. A load resets the records and emits nothing: the requests belonged to the game before it. There is no `interrupted` yet: the begin-cast flag is set by any cast the follower begins, theirs as well as ours, so a cast that began and never fired is not yet told from one that never began; that waits on tying the flag to our spell and checking it in play.

**A rule overriding the player.** When a rule pins in a fight, what its pin displaces is checked against the pins the follower had when the fight began: a displaced pin found there is `pin.overridden`, anything else `pin.released`. A rule's None does the same. A rule pinning a banned item is `ban.overridden`. A ban made in the panel on a pinned item lets the pin go, and now says so.

**Enforcement once per violation.** The watchdog puts back a pinned thing found off, and takes off a banned thing found on, every half second while the violation lasts: if the engine does not let the equip take, the same put-back repeats. A latch per follower, item and hand makes it one event per violation, cleared when the pin is next seen on or the ban's item off, and when the pin or ban goes. The pins restored after a fight are handed to the watchdog in the same pass and latched, so putting them back on is reported as the restoration, not as a violation.

## To verify in play

- A cast rule: `rule.fired` `requested`, then `rule.resolved` `cast`; a save mid-cast gives `not-cast`, `saving`, then `game.saved`.
- A rule true but blocked (the potions gone): one `rule.verdict`, not one a tick.
- An enemy rule's subject and target ids, and a leveled bandit's base against xEdit.
- A reorder mid-list: `rule.fired` still names the rule that began.
- The player's dagger pinned, a rule's bow mid-fight: `pin.overridden`; after the fight `pin.released` with `by: fight-end` and `pin.restored`, and no `pin.enforced`.
- A pinned item taken off by the console: one `pin.enforced`. A banned item equipped from the inventory: one `ban.enforced`.
- `level = warn`: the events file still has all of it.
- Two launches: the first pair archived under the right stem; a held-open log copied instead of moved; the 21st session deleting the oldest.
- The events file's modified time moving about once a second in a fight; the last event present after a normal quit.
- Events per minute over a fight with several followers, to see how long 1,000 lasts.

## Not yet

The Logs tab that reads the ring. An `interrupted` outcome for a cast. Limits per follower on the ring, if counting shows one follower crowding out the rest.
