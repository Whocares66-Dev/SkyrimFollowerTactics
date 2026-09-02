# Making a follower cast a spell

Everything below was measured on **1.6.1170** with CommonLibSSE-NG 3.7.0, in one
long session. Seven mechanisms were tried; **none of them produces a rule-timed,
animated cast yet**. This file exists so the eighth attempt starts from evidence
instead of from the same four plausible-looking dead ends.

The short version: Skyrim has **no API that makes an NPC cast a chosen spell at a
chosen moment.** That is a design property, not a gap in the bindings. NPCs
decide for themselves, and every route in is indirect.

---

## What already works

| Action | Mechanism | Status |
|---|---|---|
| Drink potion | `ActorEquipManager::EquipObject`, NPCsUsePotions' parameters | works, measured in game |
| Equip spell | `ActorEquipManager::EquipSpell` | works; her AI then casts it **when it chooses** |

`Equip spell` is the honest shipping answer today: we choose *what* she holds,
her combat AI chooses *when*. It animates and it can be interrupted, because the
game is doing the casting.

---

## The seven dead ends

### 1. `CastSpellImmediate` / `Spell.Cast` — applies, never animates

Verified from three directions:

- The CK wiki: *"This function casts the spell instantaneously. This is mainly
  desirable only for non-actors, because **it will not animate an actor**"*, and
  it recommends *"an AI package with the UseMagic procedure"* instead.
- [DynamicAnimationCasting](https://github.com/LXIV-CXXVIII/DynamicAnimationCasting)
  — a plugin whose entire job is casting spells for actors — uses exactly
  `GetMagicCaster(CastingSource::kInstant)->Cast(...)`. It fires spells **on**
  animation events; the animation is already playing, driven by something else.
- `Actor.psc` has **no `Cast` function at all**. `spell.Cast(actorRef)` is on the
  *Spell* script, and `Actor::DoCombatSpellApply` applies an effect with combat
  die-rolls, not a cast.

Reliable, predictable, no gesture, cannot be interrupted.

### 2. Animation events — outputs, not inputs

`MRh_SpellReady_Event` / `MRh_SpellFire_Event` are real, and it is tempting to
send them. They are the wrong direction.
[NPC Spell Variance](https://github.com/LeoneKingzz/NPC-Spell-Varaince-Unified)
settles it — it hooks `ProcessEvent` and *receives* them:

```
RECEIVED (the game emits): BeginCastLeft, BeginCastRight,
                           MLh_SpellFire_Event, MRh_SpellFire_Event, InterruptCast
SENT     (the mod drives): attackStop, recoilStop, bashStop, blockStop,
                           staggerStop, InterruptCast
```

Note `InterruptCast` is in **both** lists: interruption *is* a supported input.
Starting a cast is not.

That mod is also the best evidence for the general shape of the problem: a
production plugin dedicated to NPC spell usage **never initiates a cast**. It
waits for the AI to start one and then steers `caster->desiredTarget`.

### 3. `SetCurrentSpellImpl` + `RequestCastImpl` — inference, never verified

An attempt to drive the caster's state machine directly. These are `Impl`
virtuals that the game's own update loop calls, no implementation could be found
that calls them from outside, and it was never demonstrated to work.
`docs/PLAN.md` line 451 had already said to verify this API before designing
around it. Recorded here as a warning, not a technique.

### 4. UseMagic AI package — correct route, loses on priority

The documented answer, and the one the CK wiki points at. It got further than
anything else and still failed:

```
current package 000B9987 -> 000B9987 (not ours -- outranked)
```

`Actor::PutCreatedPackage` + `EvaluatePackage(immediate)` pushes the package, and
the actor's own package keeps running. Package priority comes from the *source*,
and a pushed package does not outrank one the actor owns. `createdPackage=true`
made no difference.

**This is the recommended next path.** See below.

### 5. Swapping `CombatMagicCaster::magicItem` — wrong level

The caster holds `inventoryItem` (the AI's catalogue entry) and `magicItem` (the
spell). Writing `magicItem` "took" in memory and changed nothing useful, because
the caster in question had selected a **potion**:

```
magicItem Potion of Magicka 0003EAE1 -> Fast Healing 0007231C (write took)
inventoryItem->GetMagic() = Potion of Magicka 0003EAE1
```

`AlchemyItem` derives from `MagicItem` and `CombatInventoryItemPotion` derives
from `CombatInventoryItemMagic`, so potions run through the same machinery.

### 6. Boosting `CalculateScore` — unnecessary, the spell already wins

The plan was to make our spell out-score the alternatives. Measurement killed it
in one run:

```
[spell]  Fast Healing             222.750
[potion] Potion of Healing         50.000
[potion] Potion of Minor Healing   25.000
[potion] Potion of Magicka         16.500
[potion] Potion of Minor Magicka    8.250
```

All scored in a single pass, so it is one contest — and the spell already leads
by 4.5x. Boosting would have "worked" and changed nothing.

Worth keeping: the scale is **hundreds**, not 0..1, and the score is constant
across health levels, so it is intrinsic to the item.

### 7. `CombatMagicCasterRestore::CheckStartCast` — the AI never asks

The most promising lever. The combat AI asks itself *"should I start a restore
cast now?"* through vfunc 6, and answering `true` gives rule-timed casting
through the game's own path, with animation and interruption. The hook installs,
is entered, and is safe.

It fails on something upstream:

```
combat-hook: a restore caster asked, primaryAV=25 (magicka)
queued:  2
REQUEST: 0
```

**One restore caster exists per actor value** (`primaryAV`). Only the magicka one
ever asked. The health caster never evaluated at 43% health — while the same
follower's AI healed itself from 1 HP earlier in the session, so the threshold is
real and very low.

We can answer the AI's questions. We cannot make it ask one. Interrupting is
unlikely to help: an actor whose threshold says "not hurt enough" will re-decide
the same way.

---

## Facts worth not rediscovering

- **Two spells can share a display name.** Marcurio's heal is `0007231C`; the
  vanilla one is `0002F3B8`. Both are called "Fast Healing". Compare FormIDs.
- **The UseMagic package template's named inputs** (uid = name):
  `0 Place to Travel, 1 destination, 2 Location, 3 SPELL, 4 Target,
  5 HoldWhenBlocked, 6 CastTimeMin, 7 CastTimeMax, 8 CooldownTimeMin,
  9 CooldownTimeMax, 10 NumToCastMin, 11 NumToCastMax, 12 DualCast`
  Note the map spells it **`SPELL`**, not `Spell` — compare case-insensitively.
- **The name map lives on the template**, not on packages built from it. A copy
  carries the values and a `templateParent` pointer.
- **Where a package's spell lives**, found by canary rather than guessed:
  `PTDA - Target \ Target Data \ Target` (Type = Object ID), and in memory at
  `IPackageData + 0x10 -> + 0x08`. Ints are at `CNAM - Value \ Integer`.
  Both parents are structs/unions that reject assignment.
- `MG07AncanoCastAtEye` has `NumToCastMin = 1000`, which is why Ancano casts
  forever. Copies want `1` so the package completes and normal AI resumes.
- **Dual casting is a native package input** (uid 12), so it comes free on the
  package route. Via the caster API it needs a perk most followers lack and
  multiplies the cost (~2.8x).
- `CombatController::cachedAttacker` is a **cache of** `attackerHandle`. Read the
  handle, not the cache — the cache returned `0x1` and crashed us.
- CommonLibSSE's `static_assert(sizeof(...))` confirms a struct's **size**, not
  that a named field means what it says in every state. These headers are
  reverse-engineered from a shipped binary.

---

## Recommended next path: the ESL

The package route is the conventional one, it is what the CK wiki recommends, and
it is the only mechanism that failed for a *known, addressable* reason rather
than a structural one.

`FollowerTactics.esp` already exists — ESL-flagged, 8 `UseMagic` packages
`FT_CastSlot1..8` (local FormIDs `0x800..0x807`), generated by
`tools/xedit/FT_MakePlugin.pas`. Eight because the spell lives *in* the record,
so followers cannot share one. `src/game/Packages.cpp` resolves them, calibrates
the spell field against a known canary, and can repoint it at runtime.

What is missing is **priority**. The fix, per the community write-up, is a `QUST`
with reference aliases at priority **99** — quest priority *is* package priority,
which is why a pushed package loses to a follow package.

Two open questions before committing to it:

1. Authoring a quest with aliases *and* alias package lists in xEdit. Hairier
   than a `PACK`, but `tools/xedit/` already has working scripts to build on.
2. **Filling the alias from C++.** Papyrus does it with `ReferenceAlias.ForceRefTo`;
   whether a clean native equivalent is reachable is unverified. If it is not,
   this needs a Papyrus script — which reintroduces save-game state, the thing
   the pure-C++ design was avoiding.

Also worth one cheap experiment first: if the health-restore threshold is a game
setting, changing it is far smaller than a quest. A one-time dump of GMSTs
matching `restore` / `health` / `combat` would answer that in a single run.
