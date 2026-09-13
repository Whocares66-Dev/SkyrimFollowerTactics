# Tactics in the save

A follower's rules, their switch, their pins and their bans are part of the save: one record per follower in the SKSE co-save, the `.skse` written beside every `.ess`. There is no save button and no file to manage. Loading an earlier save rolls the tactics back to what they were then, deleting a save deletes its tactics, and a save played without the mod loses nothing but them. This is the format, and when it is read and written. The code is `src/core/Profile.*` (the format, pure and tested in `tests/test_profile.cpp`) and `src/game/Profiles.*` (the records, the form names, the SKSE callbacks).

## Where

Each record is one follower's tactics as JSON text (below), keyed by the follower's **base record**: the plugin that defines them and their id within it, `Skyrim.esm-A2C94` for Lydia. Not the placed reference: "Lydia's tactics" are Lydia's whichever reference she is, a `placeatme` copy shares them, and a plugin's own id survives a load-order change where a runtime FormID does not. A follower whose base record is itself made at runtime (an `FF` id: instantiated from a leveled template, or cloned by a script) has nothing stable to key on and is keyed by reference id, which is fine inside one save; the log says so when it happens.

SKSE keeps the records under the plugin's own id (`FTAC`) and drops them on the next save if the plugin is gone. Nothing of ours goes into the `.ess` itself.

## When

- **Read** when a save loads, through the SKSE load callback, into a set of unclaimed records. A follower claims theirs the first time the tick sees them: the first tick after the load, or the moment they are recruited. Once per follower per session.
- **Written** when the game saves, through the SKSE save callback: every follower seen this session, from the session's state, plus every record still unclaimed, written back as it came. So a dismissed follower's tactics survive any number of saves made while they are away, and an autosave or quicksave carries the same records as a full save. Edits are simply the session's state until then.
- **Forgotten** before a save loads and on a new game, through the revert callback, so nothing from the last session carries into the next.

Nothing is written on a panel close or by the tick. Close the game without saving and the session's edits go with it, exactly as any other unsaved progress does.

## The record

```json
{
  "schema": 1,
  "follower": { "name": "Lydia", "form": "0xA2C94~Skyrim.esm" },
  "enabled": true,
  "rules": [
    {
      "enabled": true,
      "label": "emergency heal",
      "if": { "subject": "self", "predicate": "health-pct-below", "arg": 0.5 },
      "then": { "target": "self", "do": [ { "action": "drink-strongest", "effect": "Restore Health" } ] }
    },
    {
      "enabled": true,
      "if": { "subject": "enemy", "predicate": "resistance-pct-below", "arg": 0.25, "damage": "frost" },
      "then": { "target": "self", "do": [
        { "action": "equip-weapon", "form": "0x13989~Skyrim.esm", "hand": "both" },
        { "action": "equip-arrows", "form": "0x1397D~Skyrim.esm" }
      ] }
    }
  ],
  "pins": [
    { "form": "0x13989~Skyrim.esm", "variant": {}, "hand": "both" },
    { "form": "0x12E49~Skyrim.esm", "variant": { "enchant": [ { "effect": "0x581F7~Skyrim.esm", "mag": 25 } ], "label": "Warden" } }
  ],
  "bans": [ { "form": "0x12EB7~Skyrim.esm", "variant": { "tempering": 1.2 } }, { "form": "0x2F3B8~Skyrim.esm" } ]
}
```

| key | what |
|---|---|
| `schema` | the format's version; see below. Also the co-save record's version |
| `follower.name`, `follower.form` | who the record is for, for the log. Never read back: the record's key says whose it is |
| `enabled` | the follower's own switch, from the Tactics tab. Off silences the list without losing it |
| `rules[]` | in order; first match wins, as in the panel |
| `rules[].enabled`, `rules[].label` | the row's tick and its free text. `label` is omitted when empty |
| `if.subject`, `if.predicate` | the condition's two halves, by wire name |
| `if.arg` | the threshold, present only for predicates that take one: a 0..1 fraction for the percent ones |
| `then.do[].effect` | for the eight strongest / weakest policies, the effect's name as the game shows it (`"Restore Health"`, `"Resist Fire"`); the potion, food, ingredient or poison is chosen by it at run time |
| `if.status` | only under `status`: which status |
| `if.damage` | only under the resistance predicates, `hit-by` and `hit-type`: which kind of damage |
| `if.follower` | only when the subject is `follower`: which one, as a form |
| `then.target` | whom the actions are done on, by wire name |
| `then.follower` | only when the target is `follower`: which one |
| `then.do[]` | the actions, in order; each is done on its own availability (`core/Rule.h`) |
| `do[].action` | the action's wire name |
| `do[].form` | the spell, potion, weapon, arrows or armour it names, when it names one. Absent means none, which for the equips means "let go of every pin of that kind" |
| `do[].variant` | only under the equips: the row the rule means, as a pin's `variant` (below). Absent is the form, whichever row |
| `do[].name` | the thing's name as the panel last saw it, for any action that names a form: what the rule reads while the thing is away, and refreshed while it is there. Display only; nothing is matched by it |
| `do[].hand` | only for `equip-weapon` and `equip-spell`: `left`, `right`, `both` |
| `do[].arg` | only for `cast-spell`, and only when set: the sustain time of a concentration spell, in seconds |
| `pins[]` | the player's pins, as the panel left them: in a fight, the book remembered for after it, not the rules' fight-time pins |
| `pins[].form` | the thing pinned |
| `pins[].variant` | which row of the form (`docs/UNIQUE.md`, "The variant"): an object holding the parts the row has, each only when present, so the plain row is `{}`. Absent is the form, whichever row, which a rule's pin can be and the panel's never is |
| `variant.enchant` | the enchantment as its effects, each `{ "effect": form, "mag": number, "dur": seconds, "area": feet }`, `dur` and `area` only when not zero. The effects, not the enchantment's form: one made at the enchanting table is a form the save mints, which no plugin can name |
| `variant.tempering` | the grindstone's multiplier |
| `variant.label` | the name the player gave the copy |
| `pins[].hand` | `left`, `right`, `both`; absent for armour and ammunition, which have no hand |
| `bans[].form`, `bans[].variant` | the player's bans: forms the follower must never use, and which row, as a pin's `variant`; absent bans every row of the form. A ban is off and kept off, whichever hand |

The record carries only the fields a rule reads, so a status is written only under the `status` predicate and a hand only under the equips that take one. Absent fields read as the defaults.

**Why JSON inside a binary co-save.** The text is the same shareable format a future export will write, the serializer and its tests are one piece of code either way, and a record is read with the same lenient parser whichever build wrote it. The cost is a few hundred bytes per follower per save.

**Wire names** are the ASCII slugs in `src/core/Vocabulary.cpp`: never the panel's display text, and never renamed (the reasoning is at the top of `Vocabulary.h`). Adding one is free; renaming one silently breaks every save that used it.

**Forms** are written as `0x<local id>~<plugin>`, the plugin's own id and the plugin that defines the record, the form SPID and KID users already know. Reading resolves the plugin through the data handler, so a record is good after a load-order change without SKSE's `ResolveFormID`. A form with no plugin (made at runtime) is written as its bare id, `0xFF000DE0`, which is only good in the save it came from, which is the only place it is.

## Pins and bans, and why the mod is safe to remove

A pin is a promise about what is worn, and a load re-dresses nobody. So a saved pin is taken back only if, when the follower is first seen, they still have the thing **on**, in those hands (worn, for armour and ammunition), and it is still pinnable. Otherwise it is forgotten with an `info` line: the thing is gone, or the save was played on without the mod and the game re-dressed them in the meantime. Nothing is equipped on load.

A ban is a promise about what is **not** worn, which a load can keep for anything that still exists: a saved ban is taken back whole, and the watchdog's first pass takes the thing off if the follower has it on. A ban whose form is not in this load order is forgotten the same way, and so is a ban on a variant no row of which is carried.

**Which copy.** A pin, a ban and an equip action name one row of a form by its **variant**, one object under `variant`: the enchantment (as its effects), the tempering and the custom label, each written only when present, so the plain row is an empty object. No `variant` at all is the form, whichever row: a rule that picked no row, or a ban on the form. Why those parts and not the engine's unique id, and what follows for a pin or ban whose variant has no row left in the bag, is `docs/UNIQUE.md`.

Removing the mod: SKSE drops our co-save block on the next save, the follower keeps whatever they had on, and a later reinstall starts with no tactics. Nothing is written onto a pinned item either. The engine's prevent-removal flag was set on pins until 2026-09-04, and it outlived the mod: it does not lift on its own, and it left the engine's equip-best swap half done, with the old and the new weapon both marked equipped. Pins are now kept entirely by the mod's own equip detour, score hook and watchdog, which go away with the DLL. A save made with a build older than that still carries the flag on whatever was pinned then; unpin those in the panel once, or take the item off, and it is gone.

## Versioning, and what an unknown entry does

`schema` is bumped only for a change that a reader of the previous version could not make sense of by ignoring what it does not know: a key renamed, a value's meaning changed. Adding a key, a subject, a predicate, an action is **not** that, and is expected to be the normal case, so the number should stay at 1 for a long time.

Reading is lenient by design, so a record from another version of the mod, older or newer, gives up only what this build cannot name:

- An unknown **key** is ignored.
- A rule naming an unknown **subject, predicate, target, status, damage kind or hand** is dropped, with a warning naming it.
- An action of unknown **kind**, or naming a **form whose plugin is not loaded**, is dropped alone and its rule kept.
- A pin naming a **form whose plugin is not loaded**, or an unknown hand, is dropped alone. A record with no `pins` key has none. A `variant` of the wrong shape reads as absent, and so does a part of the wrong shape inside it; a variant with an effect whose form is not in this load order drops the pin, ban or rule alone.
- A ban that is not an object, or names a form whose plugin is not loaded, is dropped alone. A record with no `bans` key has none.
- A field of the wrong shape (`"arg": "half"`) reads as absent.
- A record that is not JSON, or not an object, reads as no record, and the log says so.
- A `schema` newer than this build's is read anyway, with a warning. So is a co-save record of a type this build does not know: skipped, with a warning.

Every drop is one `warn` line in `FollowerTactics.log`, with the follower's name and the rule's index and label. Nothing is repaired or migrated in place: the next save writes what this build kept, so a dropped rule is gone for good once the game is saved again. That is accepted; a warning in the log is the record of it.
