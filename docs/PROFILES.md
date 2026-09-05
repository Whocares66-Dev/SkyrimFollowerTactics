# Tactics on disk

One JSON file per follower holding their rules, their switch and their pins, written automatically, read automatically. There is no save button and nothing goes into the save game or the SKSE co-save. This is the format, where the files are, and when they are touched. The code is `src/core/Profile.*` (the format, pure and tested in `tests/test_profile.cpp`) and `src/game/Profiles.*` (the files and the form names).

## Where

`Data/SKSE/Plugins/FollowerTactics/followers/<Plugin>-<id>.json`, relative to the game folder, e.g. `Skyrim.esm-A2C94.json` for Lydia.

Under Mod Organizer the game runs inside its virtual filesystem, so a new file lands in the **overwrite** folder (`C:\modding\mo2\overwrite\SKSE\Plugins\FollowerTactics\followers\`). Move the files into a mod of their own if you want them managed like one; MO2 then writes updates back to that mod.

The file is named by the follower's **base record** (the `TESNPC`), not the placed reference: "Lydia's tactics" are Lydia's in every save, a `placeatme` copy of her shares them, and a plugin's own id survives a load-order change where a runtime FormID does not. A follower whose base record is itself made at runtime (an `FF` id: instantiated from a leveled template, or cloned by a script) has nothing stable to key on and gets `dynamic-<reference id>.json`, good in that save only; the log says so when it happens.

## When

- **Read** the first time the tick sees a follower: the first tick after a game loads, or the moment they are recruited. Once per follower per session.
- **Written** when the panel closes, for every follower with an unsaved edit: a rule change, the switch, or a pin, unpin or take-off from the panel. Not per edit, not per tick. Also written before the loaded files are forgotten on load game / new game, as a backstop.
- **Forgotten** on load game and new game, so the next tick reads each follower's file afresh. A different save may reuse a reference id for someone else, and a file edited by hand between saves should be seen.

The write never touches the existing file: the new text goes to `<name>.json.tmp`, is confirmed there (written without error, and the size on disk is the size of the text), then one rename replaces the old file with it (`MoveFileEx` with `REPLACE_EXISTING`, atomic on NTFS). A crash at any point leaves the last good file or the new one, never half of either. If the rename is refused, as a virtual filesystem might, the temp file is copied into place instead.

Hand-editing: edit while the game is not running, or before loading a save. The panel's next close overwrites the file whole, and a hand edit made while a save is running is not seen until a game is loaded.

## The file

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
      "then": { "target": "self", "do": [ { "action": "drink-strongest-health-potion" } ] }
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
    { "form": "0x13989~Skyrim.esm", "hand": "both" },
    { "form": "0x12E49~Skyrim.esm" }
  ]
}
```

| key | what |
|---|---|
| `schema` | the format's version; see below |
| `follower.name`, `follower.form` | who the file is for, for the reader. Written by the game, never read back: the file's name says whose it is |
| `enabled` | the follower's own switch, from the Tactics tab. Off silences the list without losing it |
| `rules[]` | in order; first match wins, as in the panel |
| `rules[].enabled`, `rules[].label` | the row's tick and its free text. `label` is omitted when empty |
| `if.subject`, `if.predicate` | the condition's two halves, by wire name |
| `if.arg` | the threshold, present only for predicates that take one: a 0..1 fraction for the percent ones, a count for `count-at-least` |
| `if.status` | only under `status`: which status |
| `if.damage` | only under the resistance predicates and `attacked-by`: which kind of damage |
| `if.follower` | only when the subject is `follower`: which one, as a form |
| `then.target` | whom the actions are done on, by wire name |
| `then.follower` | only when the target is `follower`: which one |
| `then.do[]` | the actions, in order; each is done on its own availability (`core/Rule.h`) |
| `do[].action` | the action's wire name |
| `do[].form` | the spell, potion, weapon, arrows, armour or spell it names, when it names one. Absent means none, which for the equips means "let go of every pin of that kind" |
| `do[].hand` | only for `equip-weapon` and `equip-spell`: `left`, `right`, `both` |
| `do[].arg` | only for `cast-spell`, and only when set: the sustain time of a concentration spell, in seconds |
| `pins[]` | the player's pins, as the panel left them: in a fight, the book remembered for after it, not the rules' fight-time pins |
| `pins[].form` | the thing pinned |
| `pins[].hand` | `left`, `right`, `both`; absent for armour and ammunition, which have no hand |

The file carries only the fields a rule reads, so a status is written only under the `status` predicate and a hand only under the equips that take one. Absent fields read as the defaults.

**Wire names** are the ASCII slugs in `src/core/Vocabulary.cpp`: never the panel's display text, and never renamed (the reasoning is at the top of `Vocabulary.h`). Adding one is free; renaming one silently breaks every file that used it.

**Forms** are written as `0x<local id>~<plugin>`, the plugin's own id and the plugin that defines the record, the form SPID and KID users already know. Reading resolves the plugin through the data handler, so the file is good in any load order that has the plugin. A form with no plugin (made at runtime) is written as its bare id, `0xFF000DE0`, which is only good in the save it came from.

## Pins, and why the mod is safe to remove

A pin is a promise about what is worn, and a file cannot re-dress anyone. So a saved pin is taken back only if, when the follower is first seen, they still have the thing **on**, in those hands (worn, for armour and ammunition), and it is still pinnable. Otherwise it is forgotten with an `info` line: the thing was lost, sold or swapped, or the save was played without the mod and the game re-dressed them in the meantime. Nothing is equipped on load.

That, with nothing of ours in the save game, is what makes uninstalling clean: remove the DLL and the follower keeps whatever they had on, and a later reinstall takes back only the pins that still match what they are wearing. The one trace the mod leaves in a save is the engine's own prevent-removal flag on the items it pinned (`EquipObject`'s force flag); what the engine does with that flag once nothing renews it is not verified and is in `docs/TODO.md`.

## Versioning, and what an unknown entry does

`schema` is bumped only for a change that a reader of the previous version could not make sense of by ignoring what it does not know: a key renamed, a value's meaning changed. Adding a key, a subject, a predicate, an action is **not** that, and is expected to be the normal case, so the number should stay at 1 for a long time.

Reading is lenient by design, so a file from another version of the mod, older or newer, or from another install, gives up only what this build cannot name:

- An unknown **key** is ignored.
- A rule naming an unknown **subject, predicate, target, status, damage kind or hand** is dropped, with a warning naming it.
- An action of unknown **kind**, or naming a **form whose plugin is not loaded**, is dropped alone and its rule kept.
- A pin naming a **form whose plugin is not loaded**, or an unknown hand, is dropped alone. A file with no `pins` key has none.
- A field of the wrong shape (`"arg": "half"`) reads as absent.
- A file that is not JSON, or not an object, reads as no file, and the log says so.
- A `schema` newer than this build's is read anyway, with a warning.

Every drop is one `warn` line in `FollowerTactics.log`, with the rule's index and label. Nothing is repaired or migrated in place: the next write replaces the file whole with what this build kept, so a dropped rule is gone for good the moment the player edits anything for that follower. That is accepted; a warning in the log is the record of it.
