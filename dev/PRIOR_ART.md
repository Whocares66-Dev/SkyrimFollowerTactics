# Prior-art inspection — local source and downloaded releases

Inspected 2026-09-20. This is static source, script, and plugin inspection, not an installation or in-game compatibility test. The supplied checkouts were clean and were not changed or built. Extracted research copies live under `dev/research/prior-art/`; no MO2 profile or game file was modified.

## Materials and evidence

| Material | Local location and identity |
|---|---|
| Companions' Path source | `C:\modding\companions-path`, commit `4a5475bd33e3781ac2549922c6b061a39fa98687` |
| A Fun Way To Level Followers source | `C:\modding\Follower-Leveling-System-Redone`, commit `72b09189e810888b3dcc795780780e17a0c6cc6a` |
| SFF | `C:\games\Skyrim\Mods\Simple Follower Framework 174017 2.2 2026-09-20T18-22Z JrECN21xb.7z`; version 2.2 is from the archive filename |
| AFT | `C:\games\Skyrim\Mods\Amazing Follower Tweaks-6656-3.7z`; bundled `readme_1_66.txt` identifies version 1.66, April 28, 2013; do not infer a software version of 3 from the archive name |
| NFF | `C:\games\Skyrim\Mods\Nether's Follower Framework - Universal Installer-55653-2-8-6b-1712793520.zip`; FOMOD metadata confirms 2.8.6b |
| Lucien | `C:\games\Skyrim\Mods\Lucien 20035 1.7.2 2026-06-30T22-11Z uhRBKsylB.zip`; version 1.7.2 is from the archive filename |

Archive hashes, repository identities and tool provenance pin this review (`archives.json`, `repositories.json` and `tools.json`, in the stand-alone repository's `dev/research/prior-art/`). These downloaded releases and cloned heads are not assumed to represent the latest releases or matching release/source builds.

SFF, AFT, and NFF supply Papyrus source. Lucien's BSA contains 1,533 PEX scripts and no PSC sources. Its training, spell-control, follower controller, and small function wrapper were extracted and decompiled with [Champollion 1.3.2](https://github.com/Orvid/Champollion/releases/tag/v1.3.2); disassembly was retained alongside reconstructed PSC. Decompiled code is evidence about the compiled release, not the author's original source formatting/comments. The full passive-growth system was not traced in this pass.

The plugin inventory (`plugin-summary.json`, there too) uses a [read-only header/subrecord scanner](research/prior-art/inspect_plugins.py). It validates record boundaries and compressed payload sizes, identifies masters/overrides, and extracts editor IDs; it does not resolve load-order winners or interpret dialogue conditions/VMAD. Extracted binaries/assets are local research material, excluded from version control by the research folder's `.gitignore`.

## Conclusions that affect our design

1. **Keep recruitment out of Progression.** All three recruitment frameworks override `0750BA:Skyrim.esm` (`DialogueFollower`) and distribute replacements for vanilla follower scripts. We can avoid their primary collision point entirely with our own training quest and observer/adapters.
2. **SFF is the best first integration target.** Its supplied script surface focuses on recruitment/lifecycle and exposes useful membership facts. This is evidence for a smaller overlap, not proof its uninspected DLL has no relevant behavior.
3. **NFF needs a feature-specific contract.** Its breadth creates real overlap, but “incompatible with others” is too broad. Other recruitment frameworks directly compete with it; a progression-only mod can potentially coexist when teaching, stat edits, and other overlapping behavior are coordinated.
4. **Ordinary learned spells should use actor-level addition first.** NFF and AFT use ordinary `AddSpell`; Lucien also toggles actor-added spells. The progression source's base-spell helper adds shared-form complications without being necessary for ordinary tome learning.
5. **Dynamic NPC perks have a concrete candidate implementation.** Both native progression mods add to `TESNPC` and apply perk entries to the actor. This reduces uncertainty about how to prototype perks; it does not resolve safe removal, actor isolation, or save switching.
6. **Neither progression checkout is a safe drop-in backend.** Both have source-level serialization issues and base-state restoration questions. Companions' Path additionally has broad perk-removal paths. Study their mechanisms; own and test our implementation.
7. **Lucien is an interaction reference, not a generic catalog solution.** His training and enabled-spell state are explicitly authored around him. Preserve the distinction between knowledge and use, with Tactics owning the latter.

## Companions' Path

Source root: `C:\modding\companions-path` at the commit above.

**Useful mechanisms observed:**

- `src/ActorEngine.cpp::AddPerk` checks membership, calls `base->AddPerk(perk, 1)`, then calls `ApplyPerkEntry(actor)` on each entry. `RemovePerk` removes entries on the targeted actor and removes the base-list entry. This is not vanilla `Actor::AddPerk`.
- `src/Storage.cpp` stores spent stat points and purchased perk FormIDs per actor-reference FormID in SKSE records, with resolution on load. This is closer to our desired identity than a base-name profile.
- `src/Rules.cpp::GetLevel` derives a virtual level from player or actor level and a multiplier. `ActorEngine::SetBaseStat` writes base actor values. This code does not establish a general solution for independently changing actual engine level and suppressing vanilla scaling.
- `src/PerkManager.cpp` walks actor-value perk trees and linked ranks. Tree data can be discovered dynamically; eligibility still needs fuller interpretation than copying the tree layout.

**Risks visible in the checked-out code:**

- `ActorEngine::RemovePerks` iterates the entire NPC base perk list. `PerkManager::Harmonize` removes all of it, then re-adds only recorded purchases; `RefundAll` also removes all. There is no preserved-original-perk filter in those paths. Earlier brainstorm claims about original-perk preservation came from release descriptions; **they are not supported by these particular source paths**. Do not silently equate this checkout with the advertised binary.
- `Storage::OnLoad` resolves the actor ID before reading that actor's stat/perk payload. On resolution failure, `continue` skips the loop without consuming the payload. With another actor remaining, subsequent reads start at the wrong position. The loader also does not check individual read lengths or bound serialized counts. This is a concrete parser defect in source, not an observed corrupted game save.
- `OnRevert` clears storage, but does not undo base-perk mutations. The reviewed load/reapply paths do not establish cross-save restoration. A perk already on the base makes `AddPerk` return before entry application, so process rebuild and shared-reference behavior also need measurement.
- Purchase records ownership after a void add operation, including when membership caused an early return. An already-present external perk can therefore become recorded as purchased and later removable. Our service needs a result distinguishing new grant, existing grant, and failure.
- `EventManager.cpp` calls stat harmonization on closing `StatsMenu` without a local `harmonizeStats` gate. That flag is not a blanket guarantee of no stat writes: allocation and this event still call the stat path.
- Requirement extraction reduces selected condition function IDs to a maximum comparison value. It does not evaluate the full condition expression. Tree traversal's visited-node early return can also omit additional incoming edges. Do not promise overhaul compatibility from tree discovery alone.
- *(Added 2026-09-21, while building the proof of concept.)* The function IDs it reads are partly wrong. `PerkManager.cpp::GetSkillLevelRequirement` takes 71, 73 and 277 as `GetActorValue`, `GetBaseActorValue` and a perk function; in CommonLibSSE-NG's `FUNCTION_DATA::FunctionID`, 71 is `GetInFaction`, 73 `GetFactionRank`, and 277 is `GetBaseActorValue` (`GetActorValue` is 14, `HasPerk` 448). It gives the right numbers on vanilla only because every vanilla requirement uses 277 (tests/progression/data/vanilla-perks.json: the 18 trees use only 277 and 448). A modded perk gated on a faction rank would read that rank as a skill requirement. Our reader converts every condition item by its real function and evaluates the groups, OR flags included (src/progression/game/PerkTrees.cpp, src/progression/core/Perks.cpp).

**Take:** per-reference purchase accounting, narrow engine adapter, and tree discovery as input. **Change:** selective mutations, real prerequisites, bounded deserialization, explicit engine rollback, and effect verification.

## A Fun Way To Level Followers

Source root: `C:\modding\Follower-Leveling-System-Redone` at the commit above.

- `src/serialization.cpp::AddPerkToActor` and `include/serialization.hpp::ApplyAll` use base-list insertion plus explicit entry application. The purchase list is keyed by actor FormID; repeated calls append before checking whether insertion actually succeeds.
- `include/Utility.hpp::AddBaseSpell` edits `GetActorBase()->GetSpellList()` and marks combat inventory dirty. That helper contains no spell-list `AddChange` call or spell serialization itself. Do not infer durable spell persistence from it alone; it is also a reason to prefer ordinary added spells for our teaching path.
- Skill/perk/spell milestones are defined in class templates. `src/eventSinks.cpp` awards five points on the player's level event to loaded eligible actors. This is not journey XP and would carry player crafting progression into follower growth.
- `src/followerData.cpp` reads/writes external JSON. `include/followerData.hpp` keys follower data by base EditorID, and JSON contains level/skill points. A character directory is not a save snapshot: rolling back an ESS can still read later JSON. The load callback uses a saved random UUID, while data-creation call sites in `menu.cpp` and `eventSinks.cpp` use `currentCharacterID`; those identifier paths need reconciliation before reuse.
- `PerkData::Load` has the same unresolved-actor payload-consumption problem as Companions' Path. `RevertCallback` clears bookkeeping but does not remove base perks in the reviewed path.
- `increaseActorHealthStaminaMGK` derives a new base write from `GetActorValue`, the current value. Damaged/buffed actor values therefore merit explicit tests before copying this growth pattern. The helper also calls a relocated Papyrus implementation directly; prefer a typed, verified adapter rather than inheriting runtime addresses/ABI assumptions.

**Take:** configurable milestones and the perk prototype. **Reject for our design:** external files as authoritative progression state, base EditorID identity, player-level awards, and unchecked grant accounting. No reviewed code here settles independent engine leveling safely.

## SFF 2.2

Research root: `research/prior-art/sff/`.

`Main/Source/Scripts/DialogueFollowerScript.psc` retains standard follower operations while managing a primary alias plus extra aliases. It exposes `IsManagedFollower`, `SFF_IsPrimaryFollower`, `SFF_IsExtraFollower`, and `SFF_GetAliasForActor`. Recruitment/dismissal set teammate state; waiting uses `WaitingForPlayer`. These are promising membership/lifecycle observations for an optional adapter. Some helper reads perform cleanup, so prefer passive runtime facts where sufficient rather than treating every getter as side-effect-free.

`SFF_SKSE.psc` exposes recruitment capacity/addition and essential-status helpers. Its INI includes follower limits, optional player speech/perk-based capacity, protection, sandboxing, and homes. Those capacity perks are not a follower progression/perk-distribution system. No follower XP or training system was found in the supplied Papyrus surface; the DLL was not reverse-engineered.

The main ESP has **four master-record overrides** (one quest, one topic, two responses), including `DialogueFollower`. A small override footprint supports the user's compatibility assessment, but does not mean it can be stacked with another replacement of the same quest/scripts. SFF's compiled DLL and patches remain additional surfaces to validate.

**Decision:** first multi-follower compatibility fixture is SFF alone, then SFF + Progression + Tactics. Test recruitment, waiting, primary-follower promotion, dismissal, and reload while preserving our enrollment. Do not call SFF recruitment functions merely to enroll progression.

## NFF 2.8.6b

Research root: `research/prior-art/nff/`; use its SSE main plugin when interpreting this package.

`02 Scripts Source/Scripts/FollowerAliasScript.psc::LearnSpell_Alias` demonstrates generic tome teaching: require an eligible unique NPC, resolve `Book.GetSpell()`, check knowledge and magicka, call `AddSpell`, then remove one book from the follower. It is triggered by item addition when tome-learning settings permit. Its check uses current magicka and unadjusted spell cost, and it does not check `AddSpell`'s returned success before consumption. We should improve those transaction/eligibility details.

`nwsFollower_Teach.psc` routes a dialogue fragment through the controller to `nwsFollowerControllerExScript::TeachSpell`, which teaches the player's equipped spell. That is a useful generic interaction without a dynamic named-spell dialogue catalog, but is not the requested tome-selection experience.

`nwsFollowerMCMScript.psc` distinguishes base and granted spell lists. The removal handler in `nwsFollowerMCMExScript.psc` removes from the granted list using ordinary `RemoveSpell`; this does not demonstrate arbitrary base-spell forgetting. The same script has an optional ConsoleUtil path executing `setlevel 1000 0 1 0`, and stat-modification controls. These are overlapping management tools, not independent journey progression.

The main SSE ESP has seven master-record overrides, including `DialogueFollower`. Its broader compatibility footprint comes substantially from scripts and optional installer choices, not just record count. The FOMOD additionally offers **Replace Base Dialogue Scripts**, described as 108 replacements, plus RDO/3DNPC integrations and optional spell-form edits. Do not count mutually exclusive LE/SSE branches and optional patches as one installed configuration.

**Decision:** avoid co-installing NFF with SFF/AFT for our baseline. Test Progression alongside NFF separately with overlapping spell teaching disabled, no concurrent level/class/stat edits, and no competing spell-removal ownership. If Tactics is included, audit NFF combat-role behavior separately. These are proposed compatibility conditions, not a certification; NFF's existence should not globally disable our mod, and lack of another recruitment framework does not by itself make stat ownership safe.

## AFT — historical reference

Research root: `research/prior-art/aft/`. The bundled 2013 readme supports treating this implementation as an older design constrained by its era.

`scripts/source/tweaklevelup.psc` stores manual training state in alias properties. It offers inventory objects as allocation/reset controls, handles player-level and race-change/recruitment events, and writes many stats with `SetAV` followed by conditional `ModAV` corrections. Its `original*` variables explicitly mean current managed values after an old revision, not a pristine restoration baseline. Comments describe race recalculation as a reset strategy; that is historical implementation context, not a modern restoration recipe.

`tweakfollowerspells.psc::AddSpell` teaches equipped spells through ordinary actor `AddSpell`. Names/costs/schools come from large curated arrays and DLC-specific mappings, with custom-spell confirmation logic. Its clear-spells UI explicitly distinguishes removing later additions from removing starting spells. The separate TweakMagic/tactic mechanisms belong outside our scope.

The ESP has 181 master-record overrides and supplies vanilla follower-script replacements. Its tighter coupling of recruitment, transformations, equipment, stats, and magic makes it a poor architecture template for our focused mod.

**Take:** user-facing distinctions between automatic/manual growth, learning versus tactics, and reset expectations. **Do not inherit:** broad stat correction, race reset assumptions, hardcoded name/cost arrays, recruitment replacement, or age-specific claims about engine limitations.

## Lucien 1.7.2

Research root: `research/prior-art/lucien/`; evidence below is reconstructed from the release PEX, with assembly retained.

`decompiled/jrlucientrainingscript.psc` has an explicit spell-ID dispatch to specific tome/flag properties. `LucienLearnSpellProcess` consumes the player's tome, enables that catalog spell through spell control, and adds a knowledge flag item to Lucien. Modded tomes are resolved by known plugin names and form IDs. This is a curated compatibility system, not discovery of arbitrary inventory books.

Skill lessons compare player and Lucien base values. `LucienTrainingStart` sets a skill to its current base plus an increment, then starts a blackout sequence. `LucienTrainingEnd` advances time and establishes the next lesson time. A timed cleanup removes the blackout effects. This is valuable interruption/presentation precedent, but the helper's consume-before-enable sequence is not a generic verified transaction to copy without its surrounding authored conditions.

`decompiled/jrlucienspellcontrolscript.psc` has explicit enabled-state choices, spell-specific cases, and `AddSpell`/`RemoveSpell` toggles. Knowledge flags can survive disabling the actual spell. Therefore **absence from `HasSpell` need not mean “never learned”** for a custom follower. Our inference-based enrollment could misunderstand or fight his controller.

Lucien's plugin does not override `DialogueFollower` in the header scan; it uses its own follower/training machinery. Neither its curated catalog nor its single-actor state solves generic training dialogue for every follower.

**Take:** dialogue framing, base-value lesson comparisons, explicit knowledge-versus-enabled state, and presentation cleanup. **Decision:** Lucien remains excluded from automatic progression management without a purpose-built adapter; study his experience rather than overwrite his growth/spell controller. Passive-growth cadence, full dialogue condition coverage, and other companion integrations remain untraced here.

## Next implementation decisions

- Prototype **ordinary actor-added spells** first; keep base-spell forgetting separate.
- Prototype the base-perk plus entry-application mechanism, with two shared-base references and save A/B tests from day one. Add selective refund only after ownership and effect removal are proven.
- Serialize bounded, length-delimited per-actor records; consume a complete record before discarding unresolved forms. Add fixtures with an unresolved actor before a valid actor.
- Keep earned state entirely tied to the save; external JSON is configuration/export only. Avoid current-value-to-base writes when computing permanent growth.
- Build a passive party observer first and an SFF adapter only for missing semantics. Our quest and script names must be ours.
- Keep NFF compatibility as a separate, explicit configuration test. Use AFT as a historical comparison and Lucien as an immersive interaction example.
- Continue independent-level research: **none of this inspection proves the desired general replacement of vanilla PC scaling.** The core P0 gates in [DESIGN.md](DESIGN.md) remain necessary.

The native repositories contain GPL-3.0 and MIT license files respectively, and Companions' Path's perk helper explicitly credits the other project. Preserve provenance and review any upstream-derived code before reuse rather than treating a repository's top-level label as the whole provenance story. No third-party implementation was copied into a shipping module during this review.
