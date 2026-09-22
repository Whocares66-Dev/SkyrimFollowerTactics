# How the engine answers an NPC's spells, and the view we put in front of it

Read 2026-09-21 from the unpacked executables in `C:\Modding\SkyrimVersions` (1.5.97, 1.6.1170, 1.7.104) with `tools/disasm.py`, `tools/fieldscan.py` and `tools/rtti.py`. As with perks ([ENGINE_PERKS.md](ENGINE_PERKS.md)), everything here was read from the code and none of it has been seen in play. The plugin's side of it is `src/progression/game/SpellView.cpp`. IDs are AE (1.6.1170) unless marked; 1.7.104 has the same IDs where checked.

## Why

A follower's own spells are on their NPC record, and every copy of that NPC shares the record. The engine's own per-actor storage is the *added spells* list, which `AddSpell` writes and the save keeps. `Actor::RemoveSpell` (38717) only works on that list:
- For an ability, disease or addiction (types 4, 1, 10, tested by 11290), it first ends the spell's active effects on the actor (34505). A castable spell has nothing dispelled.
- It then searches the added spells. When the spell isn't there, it returns. Only when it is found does it take the spell out of the actor's hands.

A spell on the record is still known afterwards.

So a follower can forget one of their own spells only if the shared record is edited, or if the engine's questions are answered differently. The perk view does the second, and this is the same approach for spells. It also means a spell taught from a tome no longer needs to be written to the actor.

## What reads an actor's spells

| Reader | AE | SE | Lists it reads | Used by |
|---|---|---|---|---|
| `Actor::VisitSpells(visitor)` | 38781 | 37827 | Added spells (`actor+0x190` on AE), then the record's (`[actor+0x40]+0xA8`), the race's (`[actor+0x1F8]+0x48`), and the record's leveled lists as resolved and cached in the process (39443). Each spell goes to the visitor's slot 1, and the walk continues while that returns 1 | `HasSpell` (38782, which is this walk with a finder, `HasSpellVisitor`); `AddSpell`'s already-known check (38716); the combat AI's inventory, gathered at combat start (44857) and when rebuilt (44879), both with `GatherSpellsFunctor`; the player's magic menu (52043) |
| `HasSpell` | 38782 | 37828 | Through `VisitSpells` | Condition functions, Papyrus `Actor.HasSpell` (54653 tail-calls it), and 51555 and 52011 |
| Spells by delivery, for the UseMagic procedure | 38727 | 37782 (92% alike) | Record, race, leveled. **Not the added spells** | `BGSProcedureUseMagic` (slot 0xC of its vtable, 29344 → 29346) and 40059. Each spell is kept only if `Actor::CheckCast` (Character slot 0x110) allows it |
| Cast the lists' abilities | 38753 | | Record and race, every spell through 34464 | Process setup (37177, 37178, the same functions that call `ApplyPerksFromBase`), and 37925 and 52410 |
| Dispel them | 38754 | | Record and race; abilities, diseases, addictions (types 4, 1, 10) | 7 callers |
| Per-spell setup and release (assets, by the look of 11303/11306) | 38755, 38756 | | Record and race, the same three types; 38756 also the four selected slots | Actor load and unload |
| Combat inventory, shouts | 44857, 44879 | | The record's **shouts** directly (`+0x10`, count `+0x20`) | Spells come from `VisitSpells` |
| Save a changed list | 14941 | | The whole record list, when its change flag (`kSpellList`, bit 4) is set | TESNPC's save (24387). So a record edited at run time is written into the save |

How these were found:
- `--refs` on `VisitSpells` and `HasSpell` listed their callers.
- `tools/fieldscan.py 0xA8 0x18 --size 4` found every load of `[x+0xA8]` followed by a 4-byte read of `[that+0x18]`, the spell count. Apart from load, save and false positives, that is the eight functions above.
- A second scan looked for functions handed `npc+0xA0` (the list component) that read the list through it. It found only the save (14941) and a player-only cleanup (40558).
- `tools/rtti.py` named the visitors and the procedure.

The scans are linear and heuristic. A reader that computes the address some other way would be missed, and none was searched for further.

`Actor::CheckCast` (38758) asks each of the actor's four magic casters whether it can cast the spell. Only four functions call it through the vtable: 38727 three times, 34625, and the magic menu's 52038 and 52039. Casts themselves go to the casters directly.

## The view

For each managed companion: **what the engine gives them, less the spells of their own set aside, plus the spells taught here.** Only castable spells (type 0) are in either list.

- **`VisitSpells` is detoured** with Microsoft Detours, since it isn't virtual; Follower Tactics detours its equip functions the same way. For a managed actor, the original walk runs with a filter in front of the engine's visitor. The filter passes over a set-aside spell, whichever list it comes from, and notes any taught spell it meets. After the walk, the taught spells not already met are handed to the visitor. `HasSpell`, `AddSpell`'s already-known check and the combat inventory all follow.
- **`CheckCast`, Character slot 0x110, is replaced.** It refuses a set-aside spell for a managed actor and leaves the reason untouched, as the original does when there's no caster to ask. This covers 38727, the one castable-spell reader that doesn't go through `VisitSpells`, so the UseMagic procedure won't pick a set-aside spell.
- **Out of their hands.** Setting a spell aside, and each tick after, takes it out of the actor's hands and voice with `Actor::DeselectSpell` (37820 / 38769), in case a save or a fight left it there. The same step clears it from the process's `currentPackageSpell` (`MiddleHighProcessData+0x240`). The UseMagic procedure writes its choice there only when 38727 finds a candidate, and reads it back (29346) whatever 38727 found. So a spell chosen before it was set aside would otherwise stay chosen, and be cast through the casters without `CheckCast`.
- **Withdrawn, until the fight ends.** A taught spell that is forgotten, or released by turning levelling off, is no longer handed over by `VisitSpells`. A combat inventory gathered before still lists it, though. Until the companion is out of combat, `CheckCast` refuses it, and the tick keeps it out of their hands and out of the package's choice.
- **Nothing is written to the actor or the record.** A taught spell is not added (`AddSpell` isn't called), so the save doesn't keep it. After a load the views are published again from the co-save. Turning levelling off publishes empty views, so taught spells go with the mod, and takes them out of the companions' hands.
- **Not on VR.** Neither hook is installed. Teaching falls back to `AddSpell` and forgetting to `RemoveSpell`, as before, and setting a spell aside is refused.

What the view leaves alone:
- **Abilities.** They're cast from the record by 38753 and dispelled by 38754, which walk the lists directly. The view doesn't hide them, and the panel doesn't offer to set one aside.
- **Shouts,** which the combat inventory reads from the record directly.
- **Powers** (types 2 and 3) are in the same lists as spells, and the view would hide or add one like any other. The panel just doesn't offer them yet: it lists castable spells only.

## Not verified

- **That it runs everywhere it should.** The walk is seen answering: Tactics' Magic tab reads spells through `VisitSpells`, and showed Jenassa's taught Sparks (2026-09-21). The console's `hasspell` asks the same walk. The check that asked `HasSpell` and `CheckCast` about every spell against the view, with counters of the hooks' calls, went with Progression's own pages (2026-09-21).
- **That a spell known only through the view can be cast.** The combat inventory is gathered through `VisitSpells`, so a taught Flames should be in it at the next combat start. Whether anything on the way to the cast also looks at the added spells is not known. The first session should watch a companion taught Flames, and nothing else, cast it in a fight.
- **Mid-fight changes.** A combat inventory gathered before a spell was set aside or withdrawn still lists it until it's rebuilt, and `CheckCast` isn't asked on that path. The AI can equip it; the tick takes it out of hand within a second, which interrupts that hand's cast (34427), and the AI may equip it again. Expect that churn until the fight ends.
- **Clearing the package's choice.** Writing `currentPackageSpell` to null is read from the code (29346 tests for null), and hasn't been tried.
- **UseMagic never picks a taught spell.** 38727 reads the record, race and leveled lists, but neither the added list nor the view. Vanilla treats a spell added with `AddSpell` the same way, so this isn't a regression.
- **Follower Tactics' pins.** A pin on a set-aside spell would have Tactics equip it and this tick take it off, once a second. Tactics' rule-casts go to the casters directly and don't ask `CheckCast`. Tactics should treat a spell `HasSpell` denies as gone.
- **Other sources of the same spell.** A set-aside spell is hidden from every list, so a quest that grants it again doesn't bring it back. The quest's `AddSpell` still writes it to the added list, and the save keeps it. A quest that grants a spell already taught here finds it known through the view and adds nothing; forgetting it here then takes it away entirely. A set-aside spell that a mod update removes from the record is still listed, with *Restore*.
- **Installing.** Detours writes the jump a moment before it hands back the original function, so a thread entering `VisitSpells` in between would recurse. The hook is installed at data load, before anything walks an actor's spells and before the tick starts, as Follower Tactics installs its own.
- **Readers outside the engine.** SKSE's `ActorBase.GetNthSpell` and Follower Tactics' spell walk (`ForEachSpell` in its `Sensors.cpp`) read the record and the added spells directly, so they list a set-aside spell and miss a taught one. Tactics' walk can become `actor->VisitSpells(visitor)`, a one-function change that also picks up leveled spells.

## Reproducing

```powershell
python tools/disasm.py --version 1.6.1170 38781                     # VisitSpells
python tools/disasm.py --version 1.6.1170 --refs 38781              # who walks through it
python tools/rtti.py --version 1.6.1170 210289 207689 203589        # GatherSpellsFunctor, HasSpellVisitor, BGSProcedureUseMagic
python tools/fieldscan.py --version 1.6.1170 0xA8 0x18 --size 4 --sites   # direct readers of the record's list
python tools/disasm.py --version 1.6.1170 --vtable 207886 274       # Character's vtable: 0x110 is CheckCast (38758)
python tools/disasm.py --version 1.5.97 --match 1.6.1170 38781      # SE: 37827, 79% alike (its offsets differ)
python tools/disasm.py --version 1.6.1170 38717                     # RemoveSpell: added spells only
```

`PYTHONIOENCODING=utf-8` avoids a console error when output is piped.
