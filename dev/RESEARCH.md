# Research notes — Skyrim follower tactics (compiled 2026-09-01)

Background for `PLAN.md`. Everything here is sourced; items that could not be verified from
a primary source are marked **[UNVERIFIED]** and should be re-checked before being relied on.

---

## 1. What Papyrus can and cannot do

### The follower system

- Vanilla single-follower support is the `DialogueFollower` quest → `pPlayerFollowerAlias`
  (`ReferenceAlias`) → `FollowerAliasScript`. Registration is **faction-based**:
  `CurrentFollowerFaction` (rank -1) + `PotentialFollowerFaction` (rank 0).
  [Creating a Companion](https://ck.uesp.net/wiki/Creating_a_Companion)
- `FollowerAliasScript` confirms `OnCombatStateChanged`, `OnDeath`, `OnUnload`, and
  `OnUpdateGameTime` all fire reliably on alias-filled NPCs.
  [source](https://github.com/larsepan/UMF/blob/master/FollowerAliasScript.psc)
- Multi-follower frameworks splice into this rather than replace it. EFF extends
  `DialogueFollowerScript` and reroutes `SetFollower`/`DismissFollower` into its own alias
  pool. [EFF source](https://github.com/expired6978/Extensible-Follower-Framework)
- **Implication:** per-follower state belongs on a `ReferenceAlias`, not the base NPC record,
  because aliases get quest-priority package pushes and alias-scoped events.
- **For a C++ mod, `RE::Actor::IsPlayerTeammate()` sidesteps all of this** — it is the flag
  every framework ends up setting.

### CombatStyle (CSTY)

Fields: offensive mult, defensive mult, group offensive mult, avoid threat chance, equipment
score mults, melee (power attack / bash / dual wield), close range (circle, fallback, flank
distance, stalk time), long range (strafe), flight (dragons).
[CK wiki](https://ck.uesp.net/wiki/Combat_Style)

Vanilla values are deliberately timid — offensive/defensive ≈ **0.24**, group offensive ≈
**0.51**, circle/fallback ≈ **0.22/0.11**, per
[Combat AI Uncapped](https://www.nexusmods.com/skyrimspecialedition/mods/102200), which is a
pure static CSTY record edit with no scripting, precisely because there is no runtime
per-field setter.

- `ActorBase.SetCombatStyle(CombatStyle)` exists but is on **ActorBase**, not Actor —
  likely affects all NPCs sharing the base. **[UNVERIFIED]** whether it re-evaluates
  immediately.
- No Papyrus setter for individual CSTY fields. Puppeteer rewrites the multipliers from C++.
- CSTY has **no flee threshold, no avoid-AoE field, no conditional logic of any kind.**

### AI packages

Package stack priority, highest first
([CK wiki](https://ck.uesp.net/wiki/Package_Stack)):

1. Active Scene action packages
2. **Quest alias package lists** (sorted by quest priority)
3. ActorBase package list
4. Default package list
5. System fallback

Within a list: top-to-bottom, first passing conditions wins. Separately, the AI Packages tab
exposes trigger-specific override lists including a **Combat Override** list.

- `Actor.EvaluatePackage()` forces re-derivation, but is **documented as unreliable for
  interrupting an in-flight Travel package** — the old package can keep running for a random
  duration, and if it completes first, the next package may not process.
- SKSE's `ActorUtil.AddPackageOverride(Actor, Package, priority=30, flags=0)` overrides
  *all* packages from any source, bypassing normal stack resolution.
  [signature](https://papyrus.bellcube.dev/skyrimse/script/actorutil/function/addpackageoverride/)
  A matching `RemovePackageOverride` exists by convention; **[UNVERIFIED]** exact signature.

### Driving package conditions from script

The `GetVMQuestVariable` / `GetVMScriptVariable` pattern: mark both the script and the
property `Conditional`, restrict to int/float/bool, then select the variable in the CK
condition dropdown.

**Known CK bug:** when the conditional property lives on a **ReferenceAlias**-attached
script, the CK's variable dropdown frequently fails to enumerate it, even though it works.
Workaround: type the variable name manually.
[discussion](https://www.gamesas.com/getvmscriptvariable-lists-nothing-t289278.html)
Also fails when the target reference is inside a container.

### Direct actor control — confirmed Papyrus signatures

From decompiled `Actor.psc`
([source](https://github.com/Rukan/Grimy-Skyrim-Papyrus-Source/blob/master/Actor.psc)):

```papyrus
Function StartCombat(Actor akTarget) native
Function StopCombat() native
Function SetAttackActorOnSight(bool abAttackOnSight = true) native
Function EquipItem(Form akItem, bool abPreventRemoval = false, bool abSilent = false) native
Function EquipItemEx(Form item, int equipSlot = 0, bool preventUnequip = false, bool equipSound = true) native
Function EquipSpell(Spell akSpell, int aiSource) native
Function DoCombatSpellApply(Spell akSpell, ObjectReference akTarget) native
Function EvaluatePackage() native
Function KeepOffsetFromActor(Actor arTarget, float x, float y, float z, ...) native
bool Function PathToReference(ObjectReference aTarget, float afWalkRunPercent) native
Function ForceActorValue(string asValueName, float afNewValue) native
Function SetLookAt(ObjectReference akTarget, bool abPathingLookAt = false) native
```

**Critical limitation — spellcasting.** `Spell.Cast()` and `Spell.RemoteCast()` execute
**instantaneously with no cast animation and no hand-readying check**; the CK wiki states
outright that this makes them *"unsuitable for standard actor spellcasting"* and recommends
AI packages with `UseMagic` procedures instead.
[Cast](https://ck.uesp.net/wiki/Cast_-_Spell) ·
[RemoteCast](https://ck.uesp.net/wiki/RemoteCast_-_Spell)

**Critical limitation — magic AI owns spell selection.** Vanilla combat AI spams the
highest-damage spell and will not consider a spell that deals no damage. Fixing this
required an SKSE-level solution, per
[NSV – Spell Variety AI](https://www.nexusmods.com/skyrimspecialedition/mods/132097).
`EquipSpell` controls *availability*, not *selection or timing*.

**Critical limitation — equipment is advisory.** Scripted equips get re-derived by combat AI
on its own schedule. `abPreventRemoval` locks one slot, not the AI's weapon-choice logic.

### Potions

NPC potion use is native and unexposed — there is no CSTY field and no AI package for it.
[Smart NPC Potions](https://www.nexusmods.com/skyrimspecialedition/mods/67489) is explicitly
"script-free" (SKSE plugin) for performance, and exists because vanilla NPCs can't use
player-crafted potions effectively at all.

The Papyrus trick — `Actor.EquipItem(potion, false, true)` triggers consumption — has a
**documented bug**: effects randomly re-apply hours after expiry, correlated with cell
transitions and combat starts, unresolved by the mod author who hit it
([report](https://forums.nexusmods.com/topic/13526908-potions-consumed-via-equipitem-randomly-re-apply-expired-effects/)).
Prefer the native path used by NPCsUsePotions.

### Events usable for reactive rules

| Event | Notes |
|---|---|
| `OnCombatStateChanged(Actor, int)` | Reliable on alias-filled NPCs — vanilla uses it |
| `OnDeath` / `OnDying` / `OnEnterBleedout` | Reliable |
| `OnHit(...)` | `akProjectile` is always `None` for Actors; **fires once per magic effect on top of the physical hit** — dedupe required |
| `OnMagicEffectApply` | Signature confirmed; NPC reliability not independently verified |
| `OnObjectEquipped` | Also fires for consumables |
| `OnSpellCast(Form)` | Fires **once** for concentration spells; **does not fire for Papyrus-initiated casts** |
| `OnPackageStart/End/Change` | Useful to detect whether a pushed package actually took |

**There is no native low-health event.** Health thresholds require polling.

### Papyrus performance — the actual numbers

[INI Settings (Papyrus)](https://ck.uesp.net/wiki/INI_Settings_(Papyrus)) ·
[Papyrus Tweaks NG](https://www.nexusmods.com/skyrimspecialedition/mods/77779)

- `fUpdateBudgetMS = 1.2` — main-thread Papyrus dispatch budget **per frame, shared across
  every script in the entire load order**
- `fExtraTaskletBudgetMS = 1.2`
- `iMaxAllocatedMemoryBytes = 76800` (75 KB) — exceeding it risks "stack thrashing,
  intermittent stuttering, erratic behavior and CTDs"
- Max operations per task: 100 by default
- Stack dumps trigger after ~5000 ms of sustained queue overload

Papyrus Tweaks NG's own page: the VM "was designed to be able to run on a computer that was
considered mediocre in 2011." No source gives a hard "N polling scripts is too many" — the
correct framing is budget-based, not count-based. **[UNVERIFIED]** any specific script count.

### Save bloat

Every `ReferenceAlias`/quest script instance with persistent properties is serialized into
the save. Forgotten `RegisterForSingleUpdate` loops and improperly stopped quests accumulate
as orphaned instances. Directly relevant to a "script per companion" architecture — hence
[ReSaver / FallrimTools](https://www.nexusmods.com/skyrimspecialedition/mods/5031) existing.
Another argument for keeping state in C++ and the co-save rather than in Papyrus properties.

---

## 2. Runtime and SKSE landscape (Sept 2026)

**The ecosystem is mid-scramble.** Bethesda shipped **1.7.99 on 20 Aug 2026** and
**1.7.104 on 27 Aug 2026** — one week apart. This broke essentially every SKSE-plugin mod.
SKSE64 is at **2.3.1 (27 Aug 2026)** with 1.7.104 support
([Nexus](https://www.nexusmods.com/skyrimspecialedition/mods/30379)). Address Library shipped
v12 for 1.7.99 and v13 for 1.7.104 within a day each
([files](https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files)).
[TrueHUD needed a third-party "Unofficial DLL Patch"](https://www.nexusmods.com/skyrimspecialedition/mods/189792)
because its author hadn't shipped a fix — a good illustration of the churn.

**CommonLibSSE-NG lagged the 1.7.x bump.** Community reports say CommonLib lagged the
1.7.99 bump and caused crashes. **This is the strongest single argument for pinning
1.6.1170.** `dev/COMMONLIB.md` has the fork this project builds on.

The old Unofficial Downgrade Patcher is **discontinued**; current tools are
[SDT](https://www.nexusmods.com/skyrimspecialedition/mods/188916) (downgrade from any version
to 1.6.1170) and [Reliquary](https://www.nexusmods.com/site/mods/2188).
[Nolvus's guide](https://www.nolvus.net/appendix/downgrade) still treats 1.6.1170 as the
standard modded baseline.

### Utility dependency status

| Mod | Version | Updated | Note |
|---|---|---|---|
| SPID | 7.3.3 | 2026-08-26 | On CommonLibSSE-NG, updated for 1.7.99 |
| PapyrusUtil SE | 4.8 | 2026-08-30 | AE build required for 1.6+ |
| JContainers SE | 4.2.13.1 / 4.3.1-pre | 2026-07 / 08 | 4.3.1 pre-release targets 1.7.99 |
| po3 Papyrus Extender | active | — | 374 functions, 37 events; Actor/Detection/Faction pages relevant |
| ConsoleUtilSSE, MfgFix | — | — | Prefer the **NG** forks |

### C++ APIs

Confirmed from [CommonLibSSE-NG docs](https://ng.commonlib.dev/):

```cpp
virtual float GetActorValue(ActorValue);        // RE::ActorValueOwner, Actor inherits
virtual float GetBaseActorValue(ActorValue);
virtual void  SetActorValue(ActorValue, float);
CombatGroup* GetCombatGroup() const;            // RE::Actor
void SetCombatGroup(CombatGroup*);
bool CheckValidTarget(TESObjectREFR&);
```

Verified directly against the CommonLibSSE class reference on 2026-09-01:

- `RE::Actor::BOOL_BITS::kPlayerTeammate = 1 << 26` **exists** — but as a *flag*, not a
  named `IsPlayerTeammate()` method. Combat flags `kSearchingInCombat`, `kAttackOnSight`,
  `kAttackingDisabled` are in the same enum.
- `GetCombatGroup()`, `SetCombatGroup(CombatGroup*)`, `CheckValidTarget(TESObjectREFR&)`,
  `GetTargetStatsObject()` confirmed on `RE::Actor`. **No combat-style member functions and
  no potion/drink member functions exist on Actor.**
- **Potion consumption goes through the equip manager**, signature confirmed from
  [ActorEquipManager.h](https://ng.commonlib.dev/ActorEquipManager_8h_source.html):
  ```cpp
  void ActorEquipManager::EquipObject(
      Actor* a_actor, TESBoundObject* a_object, ExtraDataList* a_extraData = nullptr,
      std::uint32_t a_count = 1, const BGSEquipSlot* a_slot = nullptr,
      bool a_queueEquip = true, bool a_forceEquip = false,
      bool a_playSounds = true, bool a_applyNow = false);
  ```
  Equipping an `AlchemyItem` through this is what triggers consumption. `UnequipObject` has
  the same shape plus `a_slotToReplace` and returns `bool`.

`RE::CombatController`, `RE::CombatGroup`, `AIProcess` (via
`GetActorRuntimeData().currentProcess`) exist. **[UNVERIFIED]**: the exact field name for
`currentCombatTarget` — AE ABI shuffles have moved fields (v3.5.4 fixed Actor vtable
offsets); verify against the live header. Same caveat for whether `boolBits` sits directly
on `Actor` or behind `GetActorRuntimeData()` on your pinned version.

### Serialization

`SKSE::GetSerializationInterface()` → `SetSaveCallback` / `SetLoadCallback` /
`SetRevertCallback` / `SetFormDeleteCallback`; `WriteRecord` or `OpenRecord` +
`WriteRecordData`. Data lands in a `.skse` co-save.

**FormIDs encode load-order index in the top byte.** If the user reorders plugins between
save and load, a stored FormID points somewhere else. `ResolveFormID(old, new)` must be
called in the load callback for every stored ID. Version your record format from day one.
[DKUtil serializable](https://github.com/gottyduke/DKUtil/wiki/5-Extra:-serializable(SKSE))
wraps the boilerplate but **does not** handle FormID resolution for you.

---

## 3. UI options compared

| Option | Fit for a rules grid | Effort | Dependency | Maintained |
|---|---|---|---|---|
| Raw SkyUI MCM | Low — fixed 2-col × 64-row grid, 128 options/page, no list widget | Medium | SkyUI | Yes |
| [MCM Helper](https://github.com/Exit-9B/MCM-Helper) | Same ceiling, JSON authoring instead of Papyrus | Low | +MCM Helper DLL | v1.6.2, Apr 2026 |
| UI Extensions SE | Medium — real ListMenu/WheelMenu | Medium | Unmaintained since 2018, author expired | **No** |
| Custom Scaleform (.swf) | High | Very high, dead toolchain | SKSE + Flash authoring | Legacy |
| **[SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352)** | **High** — real ImGui tables, drag-reorder, combos | Medium (C++) | +SKSE Menu Framework | **v3.14.1, 28 Aug 2026, 1.7 support, ~260 dependents, MIT SDK** |
| MO2 `IPluginTool` (PyQt) | High but out-of-game only | Medium | None in-game | MO2 Python API is mature |

MCM's limits are structural, not incidental: `OnPageReset` regenerates the whole page, so
every add/remove/reorder is a full re-render, and state lives entirely in your Papyrus script.
[MCM option types](https://github.com/schlangster/skyui/wiki/MCM-Option-Types)

Wheeler's author was asked to use Scaleform and declined, using ImGui — a fair signal that
ImGui has displaced Scaleform for new complex in-game UI.
SKSE Menu Framework SDK: [repo](https://github.com/Thiago099/SKSE-Menu-Framework-SDK) ·
[template](https://github.com/Thiago099/SKSE-Menu-Framework-Template).
Gotcha: debug and release DLL builds must match exactly or it CTDs.

---

## 4. Toolchain

- **CK**: Steam appid **1946180**.
  [CKPE](https://github.com/Perchik71/Creation-Kit-Platform-Extended) is the maintained
  successor to "SSE CK Fixes" — crash fixes, raised internal limits, render window fixes,
  UI QoL. Drop-in next to `CreationKit.exe`. Ships AVX2 and NoAVX2 builds.
- **`.ini`**: `bAllowMultipleMasterLoads=1`;
  `sScriptSourceFolder=".\Data\Scripts\Source"` under `[Papyrus]`.
  [MO2 CK guide](https://github.com/ModOrganizer2/modorganizer/wiki/Skyrim-Creation-Kit-SE-and-MO2)
- **Papyrus paths**: SSE wants `Data\Scripts\Source\*.psc`; FO4-derived tooling assumes
  `Source\Scripts`. SSE's compiler does not support `.ppj` project files, so every tool
  hardcodes its own guess. [papyrus-lang#95](https://github.com/joelday/papyrus-lang/issues/95)
  is still open. Keep `-i=` flags and `sScriptSourceFolder` in sync by hand.
- **VS Code Papyrus extension** ([joelday/papyrus-lang](https://github.com/joelday/papyrus-lang)):
  last stable **v3.2.0 (Mar 2023)**, one prerelease Oct 2024. Has a DAP debugger via a
  companion SKSE plugin. Still the de facto standard because nothing replaced it, but
  effectively unmaintained — **verify AE compatibility before relying on the debugger.**
- **Pyro** ([fireundubh/pyro](https://github.com/fireundubh/pyro)): maintenance status
  **[UNVERIFIED]** — no recent release confirmed. Check commit activity before adopting.
- **Champollion** decompiler: [Orvid/Champollion](https://github.com/Orvid/Champollion).
- **SKSE plugin template**: no single canonical one. CMake+vcpkg is the mainstream path —
  [SkyrimDev/HelloWorld-using-CommonLibSSE-NG](https://github.com/SkyrimDev/HelloWorld-using-CommonLibSSE-NG)
  supports `SKYRIM_FOLDER` / `SKYRIM_MODS_FOLDER` env vars for auto-deploy on build.
  An XMake camp also exists ([libxse/commonlibsse-ng-template](https://github.com/libxse/commonlibsse-ng-template)).
- **No SKSE hot reload exists.** Plugins load once at process start; the game caches vtables
  and hooks. The loop is rebuild → deploy → restart.
- **Debugging**: build Debug, launch via `skse64_loader.exe`, VS → Attach to Process →
  `SkyrimSE.exe`. Steam's DRM wrapper can crash the debugger on attach; Steamless is the
  common workaround. spdlog → `Documents\My Games\Skyrim Special Edition\SKSE\*.log`.
- **ESL limits**: 2048 new records, FormID range `xx000800`–`xx000FFF`. CELL, FACT, PACK,
  PERK, RACE, SCEN, WRLD need review before ESL-flagging.
  [ESLify](https://www.nexusmods.com/skyrimspecialedition/mods/21618) checks eligibility.
- **Papyrus "baking"**: the save stores *instantiated script state and properties*, not
  bytecode. Changing function bodies only → reloading an old save is usually fine. Changing
  a script's **shape** (properties, states, signatures) → start a new save, or accumulate
  orphaned instances and unreliable results.
- **No headless test harness exists** for Papyrus or SKSE plugins. The community answer is
  in-game console + spdlog + separating pure C++ logic from `RE::` calls so the former can be
  unit tested normally. There is no CommonLibSSE mocking layer.

---

## 5. Coverage gaps in this research

1. **r/skyrimmods returned HTTP 403 to automated fetch.** No community threads were read
   directly. The "no existing tactics mod" conclusion rests on Nexus + GitHub + web search
   only. Worth a manual look before publishing any "first of its kind" claim.
2. **GitHub API was unavailable**, so commit dates and star counts come from rendered pages.
   Treat exact dates as approximate.
3. **CommonLibSSE-NG's true current state** was not resolved at the time; `dev/COMMONLIB.md`
   settled it. Check live before any 1.7.x work.
4. Valhalla Combat, Elden Counter, TrueHUD, Wildcat appear closed-source; not exhaustively
   searched for mirrors.
5. Smart NPC Potions' implementation (SKSE vs Papyrus) was not confirmed. NPCsUsePotions is
   confirmed a C++/CommonLibNG plugin supporting SSE/AE/VR and is the better reference — but
   its README does not document the consumption technique or its follower-specific logic, so
   both claims need confirming by reading the source rather than the mod page.


## 6. Skill modifiers — what `<skill>Mod` and `<skill>PowerMod` actually do

Read from the game's own records (Skyrim.esm and Update.esm via houseCARL, 2026-09-02),
not from a wiki. Two questions: which effects write the two values, and what reads them.

**What reads them.** Every actor carries two hidden perks. Each is a list of eighteen
`ModifyActorValue` entry points, all of the form *multiply quantity by (1 + 0.01 × AV)*:

| Perk | Reads | Written by |
|---|---|---|
| `PerkSkillBoosts` (0x0CF788, overridden in Update.esm) | `<skill>Modifier` (AV 96–113) | Fortify `<skill>` **enchantments** (`EnchFortify*ConstantSelf`) and perks |
| `AlchemySkillBoosts` (0x0A725C) | `<skill>PowerModifier` (AV 135–152) | Fortify `<skill>` **potions** (`AlchFortify*`) |

**The quantity each one multiplies**, identical in both perks except for the schools:

| Skill | Entry point | Per point | Notes |
|---|---|---|---|
| One-Handed, Two-Handed, Archery | `ModAttackDamage` | +1 % | conditioned on the weapon type |
| Block | `ModPercentBlocked` | +1 % | |
| Heavy Armor, Light Armor | `ModIncomingDamage` | −1 % | **never written by vanilla** — see below |
| Smithing | `ModTemperingHealth` | +1 % | |
| Pickpocket | `ModPickpocketChance` | +1 % | |
| Lockpicking | `ModLockpickSweetSpot` | +1 % | |
| Sneak | `ModDetectionSneakSkill` | +1 % | |
| Alchemy | `ModAlchemyEffectiveness` | +1 % | |
| Speech | `ModSellPrices` +1 % **and** `ModBuyPrices` −1 % | | one value, two entry points |
| Alteration, Conjuration | Mod: `ModSpellCost` −1 % · PowerMod: `ModSpellDuration` +1 % | | |
| Destruction, Illusion, Restoration | Mod: `ModSpellCost` −1 % · PowerMod: `ModSpellMagnitude` +1 % | | |
| Enchanting | — | | in **neither** perk |

So for every skill except the five schools, Mod and PowerMod are the same bonus from two
sources and can be shown as one number (the factors multiply). For a school they are two
different things and must be shown separately: cost versus magnitude/duration.

**The three exceptions.** `EnchFortifyHeavyArmorConstantSelf`, `EnchFortifyLightArmorConstantSelf`,
`EnchFortifyEnchantingConstantSelf`, `AlchFortifyHeavyArmor`, `AlchFortifyLightArmor` and
`AlchFortifyEnchanting` all have `Archetype.ActorValue` set to the **skill itself**, not the
modifier. Fortify Heavy Armor therefore raises the Heavy Armor skill directly, which is why
the perks' `HeavyArmorModifier` / `HeavyArmorPowerModifier` entries exist but stay at zero
on a vanilla install. A mod that sets them would get a straight cut to damage taken. The
same applies to Fortify Persuasion (`Speech` directly) as opposed to Fortify Barter
(`SpeechcraftModifier`).

**Who carries the perks.** A reverse-reference scan of Skyrim.esm and Update.esm finds
`PerkSkillBoosts` on the `Player` NPC record and the 160 character-creation presets, and
`AlchemySkillBoosts` on the `Player` record only. No other NPC, race, spell or ability
references either. On the records alone, then, a follower wearing Fortify One-Handed gear
gets `OneHandedModifier` +35 and nothing turns it into damage. Not yet confirmed at runtime
(`Actor::HasPerk` on a follower would settle it); until it is, a bonus shown for a follower
is a bonus the game may not be applying.

**Consequence for the Skills tab** (`src/game/Sensors.cpp`, `BuildSkillSheet`): one
bracketed bonus per non-magic skill, "cost" and "magnitude/duration" for a school, nothing
for Enchanting, and a tooltip naming the source of each number.

## 7. Out-of-combat equipment: what decides it, and where to hook (2026-09-04)

Researched after a pinned spell lost its hand when a fight ended (Jenassa, Flames in
both hands, the right hand swapped for a sword and the watchdog fighting it at 2 Hz).

**Two different mechanisms.** In combat, equipment is chosen from `RE::CombatInventory`,
owned by the `CombatController`, through the `CombatInventoryItem` score virtuals we hook.
Out of combat there is no controller and no scored list. The rename database (meh321's
`skyrimae.rename`, ~42k names) has **no** `EquipBest`/`SelectBestWeapon` function; every
"score" name it holds is combat-side. `HighProcessData::reEquipArmorTimer` (+0x394) is
referenced nowhere outside the CommonLib header.

**The functions that are known, verified to resolve in `versionlib-1-6-1170-0-1.bin`:**

| Function | SE / AE id | Notes |
|---|---|---|
| `ActorEquipManager::EquipObject` | 37938 / 38894 | Param 7 is Papyrus `abPreventRemoval` (SKSE `PapyrusActor.cpp:338`). Non-virtual: trampoline, not a vtable write. |
| `ActorEquipManager::UnequipObject`, `EquipSpell` | 37945 / 38901, 37939 / 38895 | |
| "UpdateNPCOutfit" (community name, a `TESNPC` member) | 24234 / 418622 | `void(TESNPC*, Actor*, int64, bool checkDead, int, char)`. Re-dresses the actor in the default outfit after inventory resets and cell transitions. |
| `InventoryChanges::InitOutfitItems` / `InitLeveledItems` | 15833 / 16072, 15889 / 16129 | |

**Prior art.** *Follower Equip Control* (github.com/iRonoa9/FollowerEquipControl) detours
`EquipObject`, `UnequipObject`, `EquipSpell`, `UpdateNPCOutfit` and `InitOutfitItems`. Its
`NonCombatEquipBlocker` drops AI-originated `EquipObject` calls for weapon/shield/ammo when
the follower is out of combat with weapons sheathed, telling its own calls apart with a
`thread_local` bypass depth. It re-applies saved hand ITEMS after `UpdateNPCOutfit`, and
excludes spells. *Better Follower Equip Control* (github.com/IHateMyKite/BFEC) detours only
`UpdateNPCOutfit`; its author: "there is still no hook for function which decide what weapon
will be used". UESP: followers use the highest-damage weapon and highest-rated apparel, and
put their default equipment back on when travelling between cells.

**Conclusion.** The out-of-combat chooser itself is not located; the swap it makes is
observable and blockable where it lands, in `EquipObject`. A log-only detour there first
(flags, `IsInCombat`, return address) to confirm the post-combat sword arrives through it;
then refuse, for our followers, an AI-originated weapon/shield/torch equip into a hand a
pinned spell holds. `UpdateNPCOutfit` is the second hook if cell transitions still strip the
hand. No data-level lever exists for a spell: `ExtraCannotWear`/`ExtraShouldWear` are
undocumented, and outfits cover armour only.

### 7.1 Follower Equip Control and BFEC, read in full (2026-09-04)

Both cloned and read (FEC 2.1.0, GPL v3 -- approach only, no code; BFEC). FEC is a 40k-line
trade-menu extension: the player picks a follower's gear in the container menu, and three
layers make the choice stick -- a score BONUS on the combat AI (+600, the same
`CalculateScore` vtable slot we hook, melee/ranged/shield/staff classes only, no spell
casters), an `EquipObject`/`UnequipObject` detour that blocks or REDIRECTS AI equips, and
restore tasks after combat and after the outfit re-dress, all state SKSE-serialised.

Where ours differs, and why we keep it:

- **Zero beats bonus.** Zeroing every competitor for a pinned hand cannot be out-bid; a
  bonus can. And we cover every spell-caster class; FEC leaves spells alone entirely
  (its `EquipSpell` hook is telemetry, its hand-item restore excludes spells).
- **Shape the decision, do not redirect the act.** FEC can swap the AI's equip to the
  preferred item at the call; the AI then believes it holds what it asked for. Our score
  hook makes the AI choose the pinned thing, so plan and hands agree; the detour is a
  tripwire, logged, not a redirect.
- **Refuse only pin conflicts.** FEC blocks every out-of-combat equip of weapon/shield/
  ammo/scroll while sheathed, and suppresses `UpdateNPCOutfit` outright. Prevent-removal
  plus the watchdog has not been shown to lose, so the heavier hammers stay out.
- **Same origin test.** Both tell their own calls from the engine's by a thread-local
  depth only; neither checks return addresses. Another mod's Papyrus `EquipItem` against
  a pin is refused like the engine's. Accepted, and worth documenting for users.

Facts FEC records that we did not have: `UnequipObject` with `a_slotToReplace` set is the
unequip half of an engine weapon swap (the exemption if unequips are ever gated);
refusing or force-unequipping a BOUND weapon ends the conjuration, so bound weapons are
exempt in `Refused()`; queued equips leave `GetEquippedObject` stale within the same call
(our detour reads the hand from the request's slot, not the actor); actors with no default
outfit get outfits from AI packages through `UpdateNPCOutfit`'s third argument; FEC never
writes `CombatInventory::equippedItems`, so blocking at `EquipObject` is not known to
corrupt it, and neither codebase has evidence on whether the engine retries a refused
equip. Rule adopted from their design: never mutate the actor from inside the detour;
`Refused()` reads only.

BFEC: one detour on `UpdateNPCOutfit` that, for followers, adds each default-outfit armour
to inventory, equips it, and returns without running the engine's re-dress. Armour only.

## 8. The skill curve on armour and damage is not the player's for an NPC (2026-09-09)

The wikis' formulas -- displayed armour = base x (1 + 0.4 x skill / 100), weapon damage = base x (1 + skill / 200) -- are the PLAYER's. Read from the executable (`ActorValueOwner::GetArmorRatingSkillMultiplier`, address-library id 26424 on 1.6.1170), the multiplier is `Base + (Max - Base) x skill / 100` and it branches on `IsPlayerOwner`: one pair of game settings for the player, another for everyone else.

| | player | anyone else |
|---|---|---|
| armour | `fArmorRatingPCBase` (engine default, 1.0) to `fArmorRatingPCMax` 1.4 | `fArmorRatingBase` 1.0 to `fArmorRatingMax` **2.5** |
| damage | `fDamagePCSkillMin` (engine default) to `fDamagePCSkillMax` 1.5 | `fDamageSkillMin` to `fDamageSkillMax` (engine defaults) |

The values with a number are Skyrim.esm's game-setting records; "engine default" means the setting has no record in the plugin and holds the executable's built-in value, which `FollowerTactics.log` prints once per session (`armor: skill curve NPC ... player ...`, `damage: ...`) so the actual numbers on a load order are on record. Seen in play before the fix: Jenassa in leather (base 26 + 7 + 7 = 40) read 46 by the player's curve against the engine's 66, which is 40 x (1 + 1.5 x 0.43) at her light armour of about 43. The sheet's per-piece ratings now call the engine's multiplier for armour and use the NPC pair for damage, so they sum to the actor value. The hidden per-piece bonus (`fArmorBaseFactor`, `dev/CONDITIONS.md` 4) is the same for everyone and unaffected.
