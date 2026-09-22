# Follower Progression — implementation plan

Status: proposed implementation, following the scope decisions in [BRAINSTORM.md](BRAINSTORM.md). No progression plugin or live feasibility experiment has been implemented yet. Record/source observations, reported bugs, and untested mechanisms must remain distinguishable. Passing a source review is not passing an in-game test.

The first local prior-art inspection is complete; see [PRIOR_ART.md](PRIOR_ART.md) for pinned checkout/archive identities and findings. This establishes concrete candidates and counterexamples, not a completed engine feasibility gate.

## Product boundary

Build one composable progression system: journey-earned growth, skills and attributes, leveling, perk allocation/refunds, spell-tome teaching, and forgetting. Use conversation as the main entry point for teaching and training. Preserve the follower's starting identity by default.

Recruitment frameworks own who follows and how many followers are allowed. Follower Tactics owns spell use/avoidance and combat/equipment decisions. We observe membership and change capabilities; we do not recruit, dismiss, replace follow packages, change combat styles, or implement spell-selection AI. Standalone operation uses existing Skyrim behavior, with no dependency on Tactics or a particular recruitment framework.

Separate capability enrollment into spell management, purchased-perk management, and coordinated level/skill/attribute progression. This permits teaching while another framework owns stats. Perks may affect stats and prerequisites, so capability combinations still need validation rather than arbitrary independent switches.

## Priority risks and required evidence

P0 gates must pass for the affected feature before it enters a normal playable build. Other validated features can progress independently.

| Priority | Risk | Experiment / exit criterion | If unresolved |
|---|---|---|---|
| P0 | Engine changes and our ledger disagree after saving | Full restart/load, earlier-save rollback, save A/B switching, and missing co-save tests show no duplicate additions, lost ownership, or cross-save contamination | Disable the affected mutation path; preserve diagnostics and data instead of guessing restoration |
| P0 | NPC base mutation affects other references or templates | Target one of two references sharing a base; compare both immediately and after reload. Repeat with a templated NPC | Restrict support explicitly; do not claim per-reference isolation from a base-form edit |
| P0 | Perk exists but is ineffective, stale, or removed incorrectly | Measure an actual damage/cost effect before add, after add, after removal, after process rebuild, and after restart | Keep that perk/backend unavailable; do not replace arbitrary perks with superficially similar bonuses |
| P0 | Independent progression double-counts or loses vanilla scaling | Warrior/mage at several player levels preserve starting stats, receive precisely owned growth, and do not independently rescale | Offer separately described spell/perk functionality; supplemental training is a different mode, not a silent substitute |
| P1 | Tome selection/transaction loses books or teaches the wrong actor | Modded/localized names, duplicate input, actor switching, cancel/interruption, and autosave preserve the transaction invariant | Use a dialogue-opened item selector if fully dynamic topic text is unreliable |
| P1 | Frameworks fight over changes | Demonstrate one owner per managed domain; dismissal and framework waiting work without recruitment edits | Pause only the conflicting capability with a useful reason |
| P1 | Respec creates points or strips external abilities | Repeated refund/rebuy/reset cycles conserve all budgets and preserve baseline/external grants | Limit reset to provably owned allocations |
| P2 | Journey rewards are exploitable or unreliable | Encounter and location fixtures cover support roles, repeated events, nested locations, separation, and reload | Start with proven reward sources; defer passive travel and quest integrations |

Shouts are not part of the first release. The reported `AddShout` save/load failure motivates testing empty/missing NPC spell storage and cold reloads for our actual spell paths. It does not establish that ordinary `AddSpell` has the same bug or identify a confirmed fix.

## Architecture

Proposed layout (files below are not yet created):

| Layer | Modules | Contract |
|---|---|---|
| `src/core/` | ProgressionState, Rewards, Training, PerkGraph, Transactions, Serialization | Pure data, arithmetic, prerequisites, plans, and versioned state; no `RE::` types or engine calls |
| `src/game/` | ActorIdentity, PartyObserver, SpellAccess, PerkAccess, LevelAccess, Persistence, EventCapture | Read engine state and execute validated plans on the game thread |
| `src/integration/` | Papyrus bindings, framework adapters, optional Tactics notifications, optional devbench adapter | Narrow entry points into the same service; no alternate mutation/accounting implementation |
| `papyrus/` and plugin assets | Our quest, topics, conditions, fragments, response assets | Dialogue context and presentation; native service owns validation and accounting |
| `tests/` | Core cases and live scenarios | Budget/lifecycle tests plus measured engine outcomes |

Use an SKSE/CommonLib native service with a small authored dialogue plugin. Decide ESP/ESL packaging after the dialogue prototype establishes record requirements. Native code is a candidate backend, not a reason to bypass safe engine APIs. Compare direct implementations with Papyrus Extender before choosing a dependency. Keep runtime-specific code isolated and advertise only tested runtime versions.

Do not copy the entire Tactics project. Reuse its build/test patterns, identity resolution utilities, and serialization lifecycle concepts after reviewing their semantics and licenses. Its [profile document](PROFILES.md) deliberately shares normal base-NPC profiles across copies; progression cannot inherit that identity policy. Its documented example IDs should also be checked against actual records, rather than copied as authoritative fixtures.

## State and ownership

One progression record per actor reference in a save. Resolve placed reference identity using defining plugin/local ID and the engine/SKSE remapping appropriate to the storage format. Support dynamic references only after their identity and lifetime across saves are proven; do not promote an `FF` runtime ID to a cross-save identity. Store base identity separately to detect shared mutation scope, not as the XP key.

Each record contains:

- Enrollment and capability ownership; baseline provenance and capture version.
- Earned XP/ranks, reward history, focus, unspent skill/attribute/perk budgets.
- Allocated growth and purchased perk forms, prerequisite relationships, and actual purchase costs.
- Learned spells, approved base-spell removals/restoration data, and protected/external observations.
- Applied mutation bookkeeping, pending operation recovery data where required, and schema/backend version.

Keep logical progress in our SKSE co-save records. For every mutation, separately document what the engine writes into the ESS and what must be reconstructed. The ledger is authoritative for entitlement, but cannot simply overwrite engine state whenever there is a mismatch. A missing co-save does not mean a follower whose base form was changed is pristine.

Lifecycle design:

1. At load/revert boundaries, invalidate outstanding operation tokens and cached actor handles. Prevent queued work from the previous session from running against the new world.
2. Deserialize with size/count limits, schema checks, and form resolution. Preserve dismissed/unloaded actors' records without requiring them to be present.
3. When the world is ready, reconcile each supported actor once against the loaded ledger and actual engine state. Never repeatedly add deltas. Reconcile again only for verified lifecycle changes that rebuild relevant state.
4. Serialize consistent logical state at save. Do not execute new gameplay mutations in the save callback. If a mutation spans asynchronous engine work, define how a save during that interval is represented and recovered.
5. On save switching/new game, clear logical state and restore any nonserialized global/base mutations at the experimentally verified lifecycle point. Clearing C++ maps alone does not restore mutated engine forms.

Do not claim complete external provenance. If another mod grants a spell/perk we also granted, membership alone cannot identify ownership. Adapters may establish an explicit contract; otherwise ambiguous removal must stop rather than erase a potentially external grant. Stat restoration removes our known contribution only where the backend supports that operation safely.

Distinguish stopping enrollment, relinquishing a capability, and restoring supported changes. Disabling a feature or uninstalling must not silently mint points or attempt to roll the NPC back over unrelated edits.

## Engine feasibility work

### Spells

First trace added-spell storage, ordinary removal, base-list mutation, change flags, and save/load routines in the selected runtime. Test an NPC with no original actor effects and a mage with an existing list; inspect actual storage instead of equating “no spells listed in the plugin” with a null runtime component.

**Initial backend choice for the prototype:** ordinary actor `AddSpell`/`RemoveSpell` for spells learned here, supported by the inspected NFF/AFT/Lucien paths. Do not copy Follower-Leveling-System-Redone's base-spell helper for routine teaching. Custom follower knowledge flags, illustrated by Lucien, require explicit adapters rather than inferring learning history from current spell availability.

Expose separate operations for learning a spell and forgetting an owned learned spell. Base-spell forgetting gets a separate backend/capability gate. Read the tome's taught-spell link and use exact forms for mutations. Hand-specific NPC equivalents require explicit mappings for duplicate detection; display names are never identity.

Validate knowledge, selected/equipped state, active-effect policy, and relevant cache refreshes. Protect racial/controller/quest abilities by default. Spell teaching eligibility concerns ordinary spell records, training rules, and safe acquisition; vanilla AI willingness to select a spell is not the central criterion. Tactics controls usage. Do not implement “avoid this spell” by destructive forgetting.

### Perks

Compare reference-level and base-level paths, process entry registration, removal, and restoration. Local bindings mark base Actor AddPerk/RemovePerk as no-ops, unlike the player's added-perk storage. A native wrapper with a plausible name is not an implementation proof.

Both inspected progression projects implement `TESNPC::AddPerk` followed by per-entry `ApplyPerkEntry(actor)`; Companions' Path also removes entries before base-list removal. Use this as the first experiment. Return an explicit result distinguishing applied, already present, unsupported, and failed; record a purchase only for a successful owned grant. Never use its blanket `RemovePerks`/harmonization path. Verify second-reference behavior and entry reapplication when membership already exists.

Start with one measurable weapon-damage perk and one measurable magic perk. Record rank semantics and whether applying a higher linked rank replaces or stacks with the previous one. Tests must measure the effective outcome, not just membership. Determine which state survives process rebuild versus full restart.

Then build a small explicit catalog with skill prerequisites, linked ranks, dependencies, and protected baseline perks. Adapters can interpret overhaul-specific trees later. Validate purchase and removal plans before applying them, including cascading refunds when prerequisite skills are reset.

### Levels, skills, and attributes

Capture recruitment-era unmodified baseline separately from temporary buffs, damage, equipment effects, and class/scaling configuration. Decide what owns engine level, autocalculation, and our growth. Skill/attribute deltas must not be accidentally multiplied by ongoing PC scaling.

Compare two concrete candidates: safely own engine level/stat recalculation, or retain engine level and explicitly implement supplemental/virtual training. Measure level-dependent game behavior as well as displayed stats; virtual rank must not be mislabeled as actual engine level. Select the backend before implementing the full XP-to-stat pipeline. Shared bases, templates, and custom stat scripts are explicit support boundaries, not problems to solve by cloning NPCs without further research.

## Dialogue and transaction design

Own quest/topic records add training dialogue to eligible enrolled actors without replacing `DialogueFollower` or framework scripts. Prototype arbitrary tome names, pagination, localization, and response timing early. Prefer conversation throughout, with a filtered item selector as the practical fallback. Perk detail selection may use a compact view; dialogue should still initiate training and explain/confirm resets.

One service handles all requests, whether from Papyrus, a debug tool, or an adapter:

`preview → validate current actor/state → apply → verify → commit ledger → notify`

This describes logical stages, not a claim of engine atomicity. Each operation needs a session-bound token and idempotency key. Repeated input must not execute it twice. Recheck available tome count, eligibility, actor identity, and ownership at execution. A failed/cancelled lesson consumes no book; a successful lesson consumes exactly one and records one learned spell. Specify compensation and interrupted-save recovery based on the actual asynchronous behavior of the chosen backend. Do not refund a tome or points twice on replay.

Refund purchased perks at recorded cost. Skill retraining returns only our allocated budget and handles invalidated dependent perks; it preserves journey XP and original abilities. Full character rebuild and Legendary-style reset remain later modes requiring separate budget rules.

## Party observation and composition

Maintain separate facts: enrolled, active follower, waiting/dismissed, present, and recently participating. Teammate and faction signals are candidates; retain their source and confidence, since neither necessarily captures a framework's full semantics. Recruitment events are hints followed by current-state reconciliation. Use a bounded low-frequency fallback for missed changes; avoid scanning every actor every frame.

Initial support: ordinary persistent humanoid followers through vanilla recruitment. Next validate a lightweight multiple-follower framework. Multiple enrolled actors require no change to follower limits. Custom progression followers remain excluded unless an adapter delegates selected capabilities explicitly.

Select **SFF 2.2** as the first multi-follower fixture based on local inspection. Observe teammate/waiting state and test its primary/extra alias lifecycle; membership helpers are available if necessary, but some perform cleanup. SFF, AFT, and NFF all replace `DialogueFollower` and vanilla follower scripts. We must ship neither a replacement for those scripts nor an override of that quest. Test NFF separately with its optional teaching and overlapping stat controls coordinated; “NFF present” is not itself proof of conflict. AFT remains historical reference, not our architecture baseline.

Propose a versioned optional integration interface providing actor capabilities/state and post-commit spell/perk/stat change notifications. Notifications use resolvable identities, not persistent raw pointers. Tactics can refresh available abilities and handle stale rules on forgetting; notification consumption and its effect on rules need verification in Tactics. Failure or absence of that consumer must not undo a completed progression transaction. Do not silently rewrite the player's tactics.

With NFF/AFT or other managers, document which overlapping settings need to be disabled and test that configuration. Automatic conflict detection can identify known plugins/adapters but cannot prove arbitrary scripts have relinquished control.

## Delivery sequence

| Stage | Deliverable | Completion criterion |
|---|---|---|
| 0. Comparative research | Pinned source/record findings for the closest mods; backend candidates and unanswered questions | Each proposed mutation has an implementation reference or an explicit investigation task |
| 1. Harness and read-only service | Buildable DLL, core target, snapshots, versioned dummy ledger, test fixtures | Load/revert/save lifecycle observable; two actors and two saves remain distinct before gameplay mutations |
| 2. Risk prototypes | Minimal spell, perk, and level experiments; dialogue selection proof | P0 tests pass per backend; dynamic dialogue viability decided; unsupported scope recorded |
| 3. Spell slice | Enrollment, tome teaching, forgetting owned learned spells, dialogue, persistence | Transaction and restart cases pass with one vanilla and one modded tome; Tactics integration smoke check |
| 4. Growth and respec | Selected level backend, class-inspired focus, purchased-perk catalog, skill/perk refunds | Deterministic debug awards produce exact growth; refunds conserve budgets; effective perks verified |
| 5. Journey earning | Encounter and first-location rewards feeding the same growth service | Support roles, dismissal, separation, duplicate events, and save rollback behave as specified |
| 6. Composition and expanded scope | Multi-follower adapter, selected framework configurations, supported base-spell forgetting | Shared-reference and lifecycle tests pass for advertised configurations; known restrictions documented |

Stages are dependency-driven, not calendar estimates. Prove risky level/perk behavior in stage 2 even though their full feature arrives later. Base-spell forgetting is part of the intended scope and receives early research, but ships only with a validated backend. Passive travel XP, generalized quest rewards, broad perk overhaul support, and complete baseline rebuilding follow a stable core.

## Validation and evidence

Use houseCARL for load-order winners, NPC/class/spell/perk data, and authored dialogue inspection. It does not validate save persistence. Use the existing Tactics test tooling and optional devbench for live measurements; verify installed tool schemas and target runtime first.

For every mutation backend, exercise: initially empty and populated lists; warrior and mage; two references sharing a base; dismissal/recruitment; unload/reload; save/reload in-process; exit/relaunch/load; A/B save switching; earlier-save rollback; repeated operation; missing/unresolved forms; missing co-save; external overlapping changes. Use disposable named saves and retain the original baseline. Report crashes as observed crashes until save analysis establishes corruption.

Add explicit regression fixtures motivated by source findings: an unresolved actor record followed by a valid actor; truncated/oversized payloads; preexisting external perk purchase attempts; refund with innate perks present; stale base membership after process rebuild; and growth while hurt or temporarily buffed. Deserialize/consume the whole actor record before dropping an unresolved actor. Length-delimited records and checked reads prevent the stream misalignment seen in both reference loaders. Character-level JSON/UUID identity is insufficient for rollback; authoritative earned state must belong to the individual save.

Core tests cover conservation of earned/allocated/refunded budgets, prerequisite plans, reward deduplication, and bounded migration/error handling. Live tests cover real effects and lifecycle. Record exact build, runtime, SKSE, load order, actor/reference/base identities, commands, before/after snapshots, ESS/co-save pair, and crash logs if any. A visible spell, perk membership check, or successful in-process reload alone is not sufficient evidence.

## Should we download the prior-art mods?

**Targeted source and release inspection has now been performed.** The user cloned both progression projects into `C:\modding` and downloaded SFF/AFT/NFF/Lucien archives into `C:\games\Skyrim\Mods`. See [PRIOR_ART.md](PRIOR_ART.md) for the actual revisions, mechanisms, source defects, and remaining gaps. No release was installed or live-tested. The table below remains the investigation map; the first static pass does not imply exhaustive coverage or correspondence between a cloned head and a released binary.

| Priority | Project / source | Question to answer |
|---|---|---|
| First | [A Fun Way To Level Followers source](https://github.com/TrumanGIT/Follower-Leveling-System-Redone) | How are scaling, skill changes, perks, and save identity implemented? Which parts depend on external patching? |
| First | [Companions' Path source](https://github.com/JVeluz/companions-path) | How do virtual levels, perk refresh/removal, original stats, and save restoration work? What differs between advertised behavior and engine level? |
| First | [Papyrus Extender source](https://github.com/powerof3/PapyrusExtenderSSE) | Exact spell/base-perk mutation, serialization, and recovery mechanisms we might reuse or depend on |
| Next | [Lucien](https://josephrussellauthor.com/Lucien_Training.php) | Dialogue/topic structure, tome conditions, lesson interruption, and how a custom follower presents training; inspect available plugin/script assets |
| Next | [NFF](https://www.nexusmods.com/skyrimspecialedition/mods/55653) and [AFT](https://www.nexusmods.com/skyrimspecialedition/mods/6656) | Spell-teaching transactions and precise settings/scripts that own leveling; compatibility contracts rather than wholesale framework adoption |

For source, pin a commit and retain license notices; publicly readable code is not automatically permission to copy it. For releases, record URL, version/file ID, hash, dependencies, and whether available source actually corresponds to that release. Keep third-party research material separate from shipped assets. Inspect ESP/ESL records and distributed script source; compiled scripts may require decompilation and careful interpretation, while a DLL alone will not reveal its design as readily as source.

Download only the artifacts needed to answer these questions. Inspect archives without enabling them in the current MO2 profile. When live comparison is needed, use a dedicated profile/save, one progression manager at a time, and reproduce the same before/after and cold-reload checks. Do not mistake a mod's existence or popularity for proof of a safe implementation.

Output of this research should be a short per-project dossier: mechanism, file/function or record reference, persistence owner, known restrictions, reusable lessons, license, and remaining test. Findings should choose or reject backend candidates in stage 2. No release installation is required just to begin that work.

That first dossier is now [PRIOR_ART.md](PRIOR_ART.md). Next priorities are the minimal read-only/lifecycle harness and the P0 spell/perk/level experiments, with further source inspection targeted at remaining questions rather than downloading more frameworks. No reference inspected so far resolves general independent engine leveling or demonstrates the full save-isolation contract we require.
