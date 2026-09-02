# Making a follower cast a spell

Everything below was measured on **1.6.1170** with CommonLibSSE-NG 3.7.0, in one
long session. Seven mechanisms were tried; **none of them produces a rule-timed,
animated cast yet**. This file exists so the eighth attempt starts from evidence
instead of from the same four plausible-looking dead ends.

The short version: Skyrim has **no API that makes an NPC cast a chosen spell at a
chosen moment.** That is a design property, not a gap in the bindings. NPCs
decide for themselves, and every route in is indirect.

## Why a potion is easy and a spell is not

This is the one idea that explains every dead end below, and it is worth reading
before any of them.

**A potion is a state change. A cast is a performance.**

Drinking is an inventory operation. `ActorEquipManager::EquipObject` is the
game's own equip routine, equipping a potion consumes it, and the magic system
applies the effect at once. No animation to schedule, no AI decision, no state
machine. The engine exposes it because inventory manipulation is a normal
external operation -- quests, scripts and the player's own UI all do it.

Casting is something an actor *does over time*: charge, release, magicka drawn at
a particular instant, interruptible by a stagger, aimed at something. All of that
lives in the animation graph and the combat AI. There is no "perform this" entry
point because performing is not a state you can set.

The clincher is that the potion-equivalent for spells exists and works
perfectly -- `GetMagicCaster(kInstant)->Cast(...)` is reliable and instant. It is
exactly `EquipObject` for a potion: bypass the actor's agency and apply the
result. It simply is not a cast.

So the accurate statement is not "an NPC cannot be made to cast a spell". It is:
**the spell can be made to happen; the follower cannot be made to perform it.**
For a potion nobody notices the difference. For a spell the performance is the
whole point.

That is also why AI packages exist: behaviours go through the AI because
behaviours are the AI's job. Which is why the package route below is the
conventional answer and the recommended next path.

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

### Why it has to be a quest

Not a preference. A follower's follow behaviour comes from the vanilla
`DialogueFollower` quest's **alias package**, and alias packages outrank an
actor's own package list. So a package pushed onto the actor loses no matter
what, and the only thing that outranks a quest alias package is another one at
higher quest priority. `Actor::CheckForCurrentAliasPackage` exists as its own
vfunc, which is the engine confirming aliases are a separate, higher tier.

### How C++ would drive it

Two candidate bridges, and the second looks much better:

**`ForceRefTo` through the Papyrus VM.** `IVirtualMachine::DispatchMethodCall2`
takes a `VMHandle`, so it is reachable via the handle policy without shipping a
script of our own. But it is intricate, and the docs note ForceRefTo *"does not
yield ... the reference will be forced rapidly, not immediately"* -- an
asynchronous fill in the middle of a tactic is exactly the wrong property.

**A faction as the flag.** `Actor::AddToFaction(TESFaction*, rank)` is direct,
instant and needs no VM, and `GetFactionRank` is a standard *condition* function
that packages and aliases can read. So the bridge from our C++ to the AI is:

    rule fires  ->  AddToFaction(FT_CastNow, rank)
                ->  the alias package's condition passes
                ->  the AI runs it, casts, the package completes
    afterwards  ->  rank cleared

That keeps the alias filled once and gates on the PACKAGE's conditions, which the
AI re-evaluates continuously -- rather than trying to refill an alias, which
happens at quest start. Faction membership is the standard modding idiom for
"tag an actor so a condition can see it".

### Still open

1. Authoring `QUST` + aliases + alias package lists in xEdit. Hairier than a
   `PACK`; `tools/xedit/` has working scripts to build on.
2. How each alias acquires its follower. Eight aliases for eight followers, and
   "Find Matching Reference" fills at quest start.
3. Whether package conditions re-evaluate fast enough for a tactic to feel
   responsive.

### Rejected: changing the health-restore threshold

If the threshold that stops the health caster asking is a game setting, changing
it is technically small. It is also **global** -- it would change when every NPC
in Skyrim heals, in order to make our followers heal on time. Wrong trade for a
follower mod, and recorded as rejected so it is not rediscovered as a cheap idea.

Blast radius is a design constraint here, not an afterthought:

| approach | reaches |
|---|---|
| equip spell, instant apply | one follower, one action |
| ESL package + quest | our records, our followers |
| combat AI hooks | every actor in the game |
| GMST threshold | every actor, permanently |

The ESL route is the *most contained* mechanism that could work, not the
heaviest. It felt expensive only because authoring content is unfamiliar. The
combat hooks felt cheap and are the opposite, which is why they are now off by
default (`kEnableCombatHooks`).
