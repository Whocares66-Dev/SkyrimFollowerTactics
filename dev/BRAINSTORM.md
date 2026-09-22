# Follower Progression — brainstorm and research

Research date: 2026-09-20. This is a design exploration, not an implementation commitment. **Observed** means local records or source were inspected; **documented** means an author's description or upstream source supports it; **proposed** means our design; **unverified** means it needs a prototype or an in-game experiment. No game session was launched or game records modified for this research.

## The idea

A companion should become more capable because of the adventures we share. We should be able to give them a spell tome, discuss what they should practice, and help them reconsider their training. The result should use recognizable Skyrim skills, attributes, spells, and perks, while remaining independent of recruitment, equipment management, and combat tactics.

**Recommended direction:** journey-earned progression, dialogue-led teaching and retraining, and explicit ownership of every change. Start with ordinary humanoid followers. Keep their original identity and abilities; let the player shape subsequent growth. Follower Tactics remains an optional companion mod.

The hard part is not calculating XP. It is changing an existing NPC without double-counting vanilla growth, affecting other references of the same NPC, breaking a scripted follower, or carrying changes between saves.

### Scope decision: compose with other follower mods

Confirmed in discussion: **build a progression mod, not a new recruitment framework.** Multiple recruitment, dismissal, waiting, and follow packages are orthogonal to progression. Existing frameworks bundle these features, but we do not need to own that bundle.

| System | Responsibility |
|---|---|
| Vanilla recruitment or a follower framework | Party membership, recruitment limits, dismissal, waiting, and following behavior |
| Follower Progression | Earned growth, skills, attributes, leveling, perks, learning and forgetting spells |
| Follower Tactics | Using or avoiding available spells, combat decisions, and equipment tactics |

Progression observes party membership through a small detection/adaptation layer. Enrollment in progression is separate: dismissal pauses earning without erasing development. Support multiple enrolled followers without implementing recruitment or changing follower limits. Neither Tactics nor a particular recruitment framework should be a hard dependency.

Allow spell management without taking over leveling. Skills, attributes, and level scaling need a coordinated owner; another framework must relinquish that responsibility before we take it. Unknown ownership cannot be solved by repeatedly overwriting the other mod's values. Integration should expose state and change notifications, with adapters only where needed.

Teaching means changing knowledge, not making the follower select or avoid a spell. Tactics already addresses that behavior. Keep knowledge validity, internal-ability protection, and mutation safety here; do not implement a second combat controller. The implementation sequence and risk gates are in [DESIGN.md](DESIGN.md).

## What vanilla gives us

NPC progression is not simply the player's skill-XP system applied to another actor. The local CommonLib definitions expose NPC level/multiplier, minimum and maximum level, class skill weights, and attribute offsets. We need our own earned-XP and unspent-point accounting; an NPC's displayed skill value is not evidence of a player-style training history.

**Observed through houseCARL 1.9.0:** MO2 instance `C:\modding\MO2`, active profile `Default`, 81 resolved active plugins, including implicit masters and Creation Club content. This is a local data snapshot, not proof of the running game's state or a minimal vanilla installation. Full responses and exact queries are in [research/](research/).

| Record inspected | Finding | Design consequence |
|---|---|---|
| Lydia / `HousecarlWhiterun`, `0A2C8E:Skyrim.esm` | Winner is Skyrim.esm; player-level multiplier 1; minimum 6, maximum 50; AutoCalcStats; `CombatWarrior1H` class; health offset 50; no listed base perks or actor effects | A follower is already a configured build. Adding journey stats to continuing vanilla scaling risks double growth. Do not assume a pool of existing perk points can be refunded. |
| Lydia's factions | CurrentFollowerFaction has rank -1 in her base record; PotentialFollowerFaction has rank 0 | Being potentially recruitable, or merely having a faction entry, does not establish active participation. Check runtime membership/rank and teammate status. |
| Marcurio, `0B9980:Skyrim.esm` | Scales 1:1 from level 10 to 40; destruction-mage class; magicka offset 100; eight base spells and six perks | Starting builds vary substantially. His perks are not an earned budget we can freely refund. Several spells are explicitly left/right-hand variants. |
| `CombatWarrior1H`, `013176:Skyrim.esm` | Skill weights favor one-handed/heavy armor (3 each), archery/block (2), two-handed (1); crafting/magic weights 0; health/magicka/stamina weights 4/0/2 | Existing classes provide useful training defaults while explaining why an unmodified warrior does not grow into a mage. |
| Flames tome, `09CD51:Skyrim.esm`, and Flames, `012FCD:Skyrim.esm` | Tome's `Teaches.Spell` points to Flames; spell is aimed concentration, either hand, with two effects and a novice half-cost perk | Teaching can follow a real record link. Base cost and a spell name alone are insufficient to classify casting behavior. |
| Armsman / `Armsman00`, `0BABE4:Skyrim.esm` | Playable; one entry multiplying ModAttackDamage by 1.2; next perk `079343:Skyrim.esm`; `NumRanks=1` | Visible ranks may be separate linked records. Refunds and prerequisites need a graph, not just an integer rank on one FormID. |

The stored Lydia skill/attribute fields are also present in the dump. **Do not treat these as a measurement of her current scaled values.** Runtime actor values, temporary effects, templates, and other mods need separate inspection.

The source distinction matters too: local CommonLib has both `Actor::AddPerk` virtual calls and `TESNPC::AddPerk` base-list mutation. Their existence does not prove either call alone produces a persistent, correctly applied NPC perk. Follower Tactics' [modifier research](MODIFIERS.md) describes process-local perk entry registration and how a listed perk can differ from an effective bonus. Treat those notes as prior research, not a newly repeated runtime test.

## Prior art worth learning from

| Project | Relevant behavior documented by its author | Lesson for this mod |
|---|---|---|
| [Lucien training](https://josephrussellauthor.com/Lucien_Training.php) and [spell list](https://josephrussellauthor.com/Lucien_Spells.php) | Learns by accompanying the player; periodic growth toward the player's stronger skills; later offers dialogue lessons. Teaching uses a tome and school proficiency, with a curated list of supported spells. | The conversation is a strong model. Generalize the transaction and eligibility rules, rather than copying a character-specific trust system or a fixed spell list. |
| [Nether's Follower Framework](https://www.nexusmods.com/skyrimspecialedition/mods/55653) | Supports teaching via traded spell books or spells the player knows, including mod spells; also provides extensive follower management. | Tome teaching already has precedent. Our distinction should be a focused progression system and immersive interaction, with clear ownership when frameworks overlap. |
| [Amazing Follower Tweaks](https://www.nexusmods.com/skyrimspecialedition/mods/6656) | Offers spell teaching and managed follower stats/leveling alongside broader management. | Do not become another recruitment framework. Its managed leveling must not compete with ours. |
| [A Fun Way To Level Followers](https://www.nexusmods.com/skyrimspecialedition/mods/181813) | Allocates skill points when the player levels; configurable skill milestones grant perks/spells. Its description explicitly warns about double growth without disabling vanilla scaling. Stores NPC information in external JSON files. | Directly relevant general progression precedent. Differentiate with shared adventure rewards and dialogue. Save ownership and independent vanilla scaling deserve early tests. |
| [Companions' Path](https://www.nexusmods.com/skyrimspecialedition/mods/185158) | Describes virtual levels, stat/perk management, optional synchronization, and separate stat/perk harmonization. Changelog discusses preserving original hidden perks and fixing perk reapplication on load. | Very close prior art. Examine implementation before reinventing mutation/persistence machinery; retaining original perks is essential. Author claims about persistence are not a substitute for our own tests. |

These are feature comparisons, not compatibility certifications. The initial table describes author pages; subsequent local inspection is documented in [PRIOR_ART.md](PRIOR_ART.md). In particular, Companions' Path's checked-out harmonization code removes all base perks before restoring purchases, so the page's preservation claim must not be assumed to describe that code. We have not installed or live-tested these mods here; do not infer runtime support from Follower Tactics' support matrix.

### Local materials and conclusions from inspection

The user cloned `C:\modding\companions-path` and `C:\modding\Follower-Leveling-System-Redone`, and downloaded SFF, AFT, NFF, and Lucien into `C:\games\Skyrim\Mods`. Their pinned commits, exact archive names/hashes, inspected functions, and qualifications are in [PRIOR_ART.md](PRIOR_ART.md). Local research copies are extracted under `dev/research/prior-art/`; these were not enabled in MO2.

- **SFF first for composition:** supplied scripts retain teammate/waiting signals and expose membership helpers, with no training system found in that script surface. It still replaces `DialogueFollower`, as do AFT and NFF; our independent quest avoids that shared ownership. SFF's DLL was not reverse-engineered, and integration still needs a live test.
- **NFF by feature contract:** its tome learning uses `Book.GetSpell()` and ordinary actor `AddSpell`; its granted/base spell UI supports the storage distinction. Recruitment, optional script replacements, spell learning, and stat edits explain overlap. This does not establish incompatibility with every focused mod. Test it separately from SFF/AFT, with explicitly coordinated features.
- **AFT as historical evidence:** its bundled readme identifies a 2013 implementation. Its large spell maps and broad stat correction are useful context, not current best-practice recommendations.
- **Native perk feasibility is more concrete:** both progression checkouts modify the NPC base perk list and apply entries to the actor. Both also have unresolved-actor deserialization defects and no demonstrated base-state rollback in the inspected revert paths. Borrow the experiment, not an unverified backend.
- **Lucien preserves knowledge separately from spell availability:** his compiled training/spell-control scripts use a curated spell/tome/flag mapping and enable/disable logic. This supports our division between progression and Tactics, and argues against automatically managing custom followers' internal knowledge state.

Independent engine leveling, safe base-spell removal, and cold-load/save-switch isolation remain unproven. Full source/record findings supersede tentative assumptions from mod descriptions where they differ.

## How should a follower earn progress?

Three plausible models:

| Model | Strength | Cost / weakness |
|---|---|---|
| Actual skill use | Closest to the player's learn-by-doing fantasy | Hit/spell events do not automatically reveal useful damage, healing, or skill XP. Rewards favor rapid attacks and damage dealers unless carefully normalized. |
| Shared adventure XP | Rewards exploration, tanks, healers, and companions who accompany us | Requires rules for participation, duplicate rewards, waiting, and discovery semantics. |
| Player-level synchronization | Simple pacing and catch-up | The player's crafting can advance a follower who did nothing; weakest fit for this request. |

**Proposed default: shared adventure XP, distributed into a chosen training focus.** Actual combat behavior can later bias skill allocation, without determining who deserves the encounter reward.

- **Encounters:** award a bounded amount after a meaningful hostile encounter ends. Record participation over the encounter, so a healer, archer, or follower recovering from bleedout still counts. Avoid last-hit attribution. Exclude sparring with allies and trivial repeat farming; summoned creatures and resurrected enemies require explicit rules.
- **Exploration:** award a first-visit bonus per follower for a meaningful named location. A new companion can learn from an old dungeon. Normalize child cells/locations into a deliberate reward identity; walking through three doors must not create three discoveries.
- **Dungeon completion:** a larger, once-per-follower reward when the follower participated. Reading a location's cleared state is not the same as observing a new clear. Detect transitions or use a verified event; do not award all already-cleared locations at enrollment.
- **Travel:** optional small, capped credit for active adventuring together. Exclude menus, wait/sleep, loading, idle loops, and fast-travel time jumps. Distance alone is farmable; raw calendar time is worse. Start with this disabled until the encounter/exploration loop feels right.
- **Quests:** optional later integration for quests the follower accompanied. A generic quest completion can be hidden bookkeeping and is not sufficient evidence of a shared accomplishment.

Prototype tuning only: encounter 10 XP, first meaningful visit 20, first dungeon completion 50; next journey rank costs `100 + 25 * earnedRanks`. Tune against short and long dungeons, stealth runs, and parties of different sizes. Give each eligible follower a full share initially; dividing by party size discourages bringing companions. Balance overall party power separately through pacing/caps.

Eligibility should combine explicit enrollment with live recruitment/teammate information, presence, and a short participation history. Loaded proximity alone would reward a homebound follower while the player fights outside. An unloaded actor should not accrue guessed combat XP. Dismissal pauses earning but preserves growth; temporary separation at a door should have a grace period.

Record awarded encounter/location identities in the save. Death notifications can repeat, cell transitions can recur, and loading a save must restore the award history to that save's moment. Never replay accumulated real-time or calendar-time gaps after loading.

## What actually levels up?

Keep three concepts distinct: **earned journey progress**, **skill/attribute growth**, and **the engine's actor level**. A virtual rank does not necessarily change level-dependent spells, encounter logic, or `GetLevel`.

Two useful implementation candidates:

1. **Own progression after enrollment (preferred product behavior).** Preserve a baseline, stop the follower's independent PC scaling through a proven mechanism, and apply earned growth. The engine-level update and stat recalculation path are a feasibility gate. Do not blindly change shared NPC records or call `SetLevel` and assume recalculation is correct.
2. **Supplement vanilla progression (fallback / optional mode).** Keep normal NPC leveling, award only a limited supplemental training budget, and clearly describe this as bonus training. It is easier to coexist with some mods but does not deliver fully independent follower leveling. Do not silently present it as equivalent to option 1.

For an MVP, favor focus-based growth over eighteen separate XP bars. Conversation choices might be “Practice sword and shield,” “Study destruction magic,” or “Keep developing your strengths.” The last uses the follower's existing class as a starting preference. A warrior can eventually become a mage, but requires skill, magicka, and suitable spells rather than just a label change.

Use Skyrim's familiar attributes and skill range as initial defaults. Leave crafting, pickpocket, lockpicking, and speech outside automatic training unless we later implement corresponding follower activities. Do not award smithing skill because the player smithed nearby.

Avoid multiplying NPC autocalculated health with player-style attribute awards by accident. First measure baseline and growth curves on a warrior and a mage at several levels. Preserve experienced recruits rather than resetting everyone to level 1. Propose gradual catch-up for companions who actually adventure, not instant retroactive points on recruitment.

## Teaching through conversation

Desired flow:

> Player: “I'd like to teach you something.”  
> Player: “Study this spell tome.”  
> Player selects an eligible tome.  
> Follower: “Let's give it a try.”  
> One tome is consumed on successful learning.

**Proposed rules:** player holds the tome; knowing the spell personally is optional, because the book provides the instruction. Require the follower's relevant school proficiency and a practical magicka budget. The familiar 0/25/50/75/100 tiers are a starting teaching policy, not a claim that Skyrim prevents everyone from casting below those skill values. Resolve actual spell/effect data and permit compatibility overrides for spells with unusual schools or costs.

Enumerate inventory books and follow their taught-spell link; do not match English names, assume a FormID range, or hardcode vanilla tome lists. Group duplicate tomes of the same spell. Exclude already-known spells, abilities, powers, quest/controller spells, and non-spell books. A normal tome is evidence of player-facing intent, **not proof that NPC AI can use its spell**. Start with a tested allowlist of effect/archetype patterns plus explicit overrides; unknown scripted spells should need an opt-in compatibility rule.

Adding a spell does not change combat style or guarantee autonomous use. That is a composition boundary, not a requirement to build spell-selection AI here: Follower Tactics owns using and avoiding spells. Verify that taught forms remain valid and that forgetting refreshes relevant engine state; use representative casting checks to catch bad mutations. Do not gate all teaching on vanilla AI preference or promise support for every scripted spell merely because Tactics is installed.

Marcurio's inspected spell list highlights another issue: an NPC can know a hand-specific variant with the same display name as a player-tome spell. Use exact form identity for mutations, and a deliberate equivalence mapping for duplicate-learning checks where appropriate. Neither comparing translated names nor treating all similarly named spells as interchangeable is safe.

Commit teaching as one logical operation: revalidate actor/tome/eligibility, add and verify the spell, consume exactly one tome, and record provenance. Handle failure with a defined rollback; cancellation or a failed add must not consume a book. Test interruptions, duplicate dialogue activation, and saving between stages. Protect quest-marked inventory copies.

### Making the dialogue general

**Proposed architecture:** a small quest/dialogue plugin with Papyrus fragments calling the native progression service. Unlike Follower Tactics' DLL-only package, authored dialogue is a good reason to ship an ESP, potentially ESL-flagged after checking record requirements. Use our own quest and eligibility conditions rather than replacing `DialogueFollower` or its core scripts.

A generic topic such as “Study this tome” is easy to author; an arbitrary, localized list of hundreds of modded spell names in dialogue is a separate engineering problem. Prototype that before promising a completely menu-free catalog.

Candidate presentations, in preference order:

1. **Dialogue list with paged candidates:** bounded topic slots whose displayed tome names and selected spell stay synchronized. Investigate text substitution or runtime topic support; no verified generic implementation yet. Recheck when switching actors and avoid stale shared topic state.
2. **Dialogue plus a standard item-selection interaction:** conversation opens a filtered book selection, then returns to the follower's confirmation. This preserves the social framing and uses inventory literacy, but the selection surface needs implementation and testing.
3. **Pure dialogue for a finite initial spell catalog:** feasible fallback, with generated records/patches for additional packs. Honest about limited generality.

Avoid requiring a large management panel to teach one spell. For perks, a compact optional skill/perk view may be more usable than dozens of dialogue pages; start and confirm the action in conversation. Provide a dialogue path for focus and reset even if detailed allocation uses a view.

Voice coverage is another real constraint. Prototype brief original/generic responses for common follower voice types and test subtitle timing. Silent custom voices need a documented fallback; do not assume they inherit vanilla dialogue responses. Do not commit to synthesized imitations as a dependency.

## Forgetting spells

Offer “Let's review your spells” → selection → “Forget this spell.” Explain that relearning normally costs another tome. No automatic tome refund; otherwise forgetting becomes a book generator.

Distinguish three sources:

- **Learned here:** reversible changes tracked by this mod; first milestone.
- **Built-in ordinary spells:** desired eventual support, but a separate mutation path and test gate.
- **Race, quest, perk-granted, or controller abilities:** protected by default. Other systems may immediately re-add them or depend on them.

Upstream [Papyrus Extender Actor source](https://raw.githubusercontent.com/powerof3/PapyrusExtenderSSE/master/src/Papyrus/Functions/Actor.cpp) explicitly separates added spells from base-list removal. Its base-removal implementation accesses the NPC spell list, handles active effects and selection, marks combat inventory dirty, and marks a spell-list change. This supports feasibility, while exposing shared-base and save-state concerns. Its perk functions also route through a serialization manager. These are source observations, not a tested dependency choice.

For forgetting, verify knowledge, equipped/selected spell state, AI choice, active effects, and persistence. Removing knowledge need not erase every already-created summon or effect; define that behavior deliberately. Do not promise reversible base-spell editing for arbitrary templated or duplicated NPCs until isolation is demonstrated.

**Save-safety qualification from follow-up discussion:** a base-list edit changes the loaded NPC form, not the ESM/ESP file. Marking a spell-list change allows engine persistence, but is not evidence that the mutation saves and reloads correctly. NPC `AddShout` has longstanding [firsthand reports of crashes loading subsequent saves](https://www.reddit.com/r/skyrim/comments/oelu7a/). The precise claim that an initially absent spell list causes the failure remains unverified, as does whether reported files are malformed or trigger a load-time engine bug. Test initially empty/missing storage separately and include full process restarts. Shout teaching is outside the initial spell-tome scope; ordinary spells must use their own tested path.

## Perks and retraining

Offer recognizable vanilla perks, but only those proven useful for NPCs. A playable perk can depend on player-only UI, crafting, activation, or scripts. Being present in a list, or returning true from a membership check, is insufficient: measure damage, spell cost, armor, or the actual ability.

Start with a small combat/magic catalog; expand after testing. Respect skill requirements, linked ranks, prerequisite branches, exclusions, and conditions evaluated with the follower as the relevant actor. Loaded perk records are input data; arbitrary perk overhauls still need adapters. Never infer a complete tree solely from `NextPerk`.

**Points belong to our ledger.** Keep innate/external perks distinct from purchased perks. Do not convert every perk a follower happens to have into spendable points. Hidden NPC balancing perks and SPID grants are not free respec currency.

Separate three operations in conversation:

| Operation | Effect |
|---|---|
| Change training focus | Future skill growth changes; no refunds needed. |
| Refund purchased perks | Remove our purchased ranks in dependency order; return exactly their recorded cost; leave skill values and original perks intact. |
| Retrain skills | Return our allocated skill budget to an unspent pool and reduce those skills to the supported baseline; refund dependent purchased perks that become invalid. Preserve journey XP. |

The third operation directly supports “reset skills and redistribute.” It is deliberately not an automatic reset of every original skill to 15. A later full rebuild mode would need explicit baseline replacement rules and treatment of innate/external perks. Likewise, a Legendary-style reset-to-15 loop is a separate design: repeated resets must not manufacture new journey XP or points from previously earned growth.

Preview what changes and what is refunded, then confirm at rest. A gold cost or training interval can add weight; avoid rare consumables in the first prototype. Lowering magic skills may make spells unsuitable for teaching without erasing knowledge already acquired; decide whether they remain castable, rather than silently forgetting them during a reset.

## Ownership, persistence, and compatibility

Use a per-save, per-actor ledger: enrollment baseline, earned XP, reward identities, skill allocations, unspent points, purchased perk ranks/costs, learned spells, approved suppressed spells, and mutation/schema version. Store stable form identities with load-order remapping; do not use actor name, a raw pointer, or one global JSON file as save identity.

Reapply changes idempotently: desired owned state versus what is currently applied, not “add another +5 on load.” Save A → save B in the same process is a mandatory test, especially for base-record mutations. Missing plugins/forms should quarantine affected entries with diagnostics instead of redirecting refunds or changes to another form.

Actor values do not generally retain a trustworthy per-mod contribution list; a snapshot is not sufficient ownership. Track our deltas and detect incompatible external edits. Do not continually overwrite everything to force a baseline, or remove an external grant just because we once granted the same perk/spell. Ambiguous shared ownership needs a compatibility policy, not guessed provenance.

Recruitment and progression should remain separate. Adapters can recognize vanilla followers, teammate-based frameworks, and explicit imports. Exclude summons, temporary quest actors, creatures, and highly scripted followers by default until supported. Lucien and other custom progression followers should keep their own progression unless a dedicated adapter exists.

With NFF/AFT or another stat manager, choose one owner for levels/stats. Spell teaching may coexist if ownership is clear; competing respec/forget systems require caution. SPID and SkyPatcher are useful data/distribution tools, not substitutes for a per-save earned-progress ledger. Test ordering against their grants and changes.

Do not promise mid-save uninstall safety. Provide a “stop managing / restore supported changes” path and document its limitations after testing; keep pre-enrollment saves for development. Restoration must preserve unrelated changes made since enrollment.

## Reusing Follower Tactics and local tooling

Local inspection found:

| Resource | Reuse / investigation |
|---|---|
| [houseCARL bundled skill](../../../modding/houseCARL/codex/housecarl/SKILL.md) and `C:\modding\houseCARL\housecarl\server\housecarl-mcp.exe` | The executable works via stdio MCP, even though no houseCARL tools were exposed directly in this session. Initialized it and used read-only status, query, and record tools. No installation or global configuration changes were needed. |
| [Research driver](research/inspect_records.py) | Reproduces the local record reads. Uses the specified MO2 instance and writes cache/evidence under this project's research folder. Source plugins remain untouched. |
| [Tactics profiles](PROFILES.md), `src/game/Profiles.cpp` | Starting point for SKSE serialization, identity remapping, and load/revert lifecycle review; progression needs its own ledger and stricter mutation restoration. |
| `src/game/Sensors.cpp`, `Sensors.h`, `Tactics.cpp` | Reuse follower identification and spell/skill inspection concepts; check current behavior rather than assuming every helper has the right progression semantics. |
| [Magic notes](MAGIC.md), [modifier notes](MODIFIERS.md) | Existing research on casting and effective bonuses. Particularly valuable for proving learned spells and purchased perks actually work. |
| [devbench investigation](DEVBENCH.md), local checkout `C:\modding\devbench` | Optional live test layer. The Tactics notes explicitly distinguish researched tooling from installed/integrated support. Inspect health/schema before using a running server; do not assume one exists. |
| `tools/check_install.py`, `addrlib.py`, `disasm.py`, `livedisasm.py`, `ess_scan.py`; CMake presets and tests | Useful runtime identification, engine investigation, save inspection, and build/test patterns. Adopt selectively, with their license notices. |

houseCARL is for static records, load-order winners, scripts/assets, and schema research. It cannot prove runtime scaling, spell use, perk application, or save restoration. devbench can help stage and observe those live experiments. Keep tests on a named disposable development save and record runtime, SKSE, load order, and plugin versions.

Preserve Tactics' useful architectural split: pure progression/accounting rules in `core`, all engine/Papyrus/dialogue integration in `game`. Headless tests should cover reward deduplication, budget conservation, dependency-aware refunds, and serialization migrations. Live tests should cover engine effects and lifecycle behavior. A progression DLL should not require Tactics or SKSE Menu Framework solely because the old project uses them.

## Feasibility experiments, in order

1. **Spell transaction and dialogue:** teach a known supported spell through conversation to one mage and one warrior; verify book consumption, knowledge, forgetting, cancellation, save/load including process restart, dismissal, and interrupted conversations. Prove dynamic tome selection with one modded book. Spell-selection behavior remains Tactics' responsibility.
2. **Perk application and removal:** test a simple damage perk and a magic-cost perk; compare actual engine outcomes before/after, then unload/reload. Include a second reference of the same base NPC to expose spillover. Repeat with an external perk distributor.
3. **Level ownership:** on Lydia and Marcurio, record actual levels and unmodified stats before/after player leveling, earned growth, reload, and dismissal. Test capped, fixed-level, and templated NPCs. Decide whether independent engine leveling can be supported without damaging identity or persistence.
4. **Journey reward detection:** capture actual combat/death/location signals; verify fleeing, bleedout, support-only participation, fast travel, cleared-location revisits, nested cells, and save rollback. Add passive travel only after these are reliable.
5. **Respec and isolation:** skill reset plus dependent perk refund; zero budget gain over repeated cycles; save A/B switching; missing forms; external stat edits; two followers sharing a base; mod shutdown/restoration.

First playable slice: explicit enrollment, one training focus, encounter/first-visit XP, a small proven spell catalog through dialogue, forgetting spells learned here, and a small refundable perk catalog. Full base-spell forgetting, broad perk overhaul support, and arbitrary follower rebuilds remain explicit follow-on goals, not silently dropped requirements.

## Decisions to revisit after prototypes

- Must earned progression change the engine actor level, or is supplemental training acceptable for a first release?
- Should growth follow actual skill use, a chosen focus, or a blend once reliable event attribution exists?
- Should followers be capped at the player's level, slowly catch up, or be allowed to surpass the player?
- Is a standard item selector acceptable inside a teaching conversation if arbitrary spell-name dialogue is fragile?
- Should retraining preserve recruitment-era skills, or eventually allow an explicit full rebuild of supported NPCs?
- How much voice coverage is needed for an immersive first release?

The best next investment is a narrow dialogue/spell prototype paired with a perk/level lifecycle experiment. Those resolve the expensive uncertainties before balancing a large progression system.
