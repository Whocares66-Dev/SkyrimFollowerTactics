# CommonLibSSE — what it is, which fork, and why

## What it does

SKSE gets your DLL loaded into Skyrim and gives you a handful of services: a messaging bus,
save-game serialization, Papyrus function registration, and a trampoline for hooks. What it
does **not** give you is any knowledge of Skyrim's internals. Skyrim ships as a stripped
binary with no headers, so from C++'s point of view an `Actor` is just an address.

CommonLibSSE is the community's reverse-engineered header library that fills that gap: class
definitions, inheritance and vtable layouts, member offsets, enums, and helper wrappers for
thousands of the engine's internal types. Every API this project's plan depends on is a
CommonLibSSE definition:

| We need | CommonLibSSE gives us |
|---|---|
| Read a follower's health | `RE::ActorValueOwner::GetActorValue(ActorValue::kHealth)` |
| Identify followers | `RE::Actor::BOOL_BITS::kPlayerTeammate` |
| Make an NPC drink a potion | `RE::ActorEquipManager::EquipObject(...)` |
| Read combat state | `RE::Actor::GetCombatGroup()`, `AIProcess` |

Without it we would be hand-computing struct offsets against a specific build and redoing
that work every patch. It is not optional in any practical sense — it is the only reason
writing an SKSE plugin is a normal C++ job rather than a reverse-engineering project.

## Which fork (checked 2026-09-01)

There are four, and the one most tutorials point at is no longer the live one:

| Fork | Last commit | Verdict |
|---|---|---|
| [CharmedBaryon/CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) | 2024-09-04 | Stale ~2 years. What most templates point at. |
| [alandtse/CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG) | **2026-08-30, v7.0.0** | The live NG fork. Multi-runtime, single DLL for SE/AE/VR. |
| [powerof3/CommonLibSSE](https://github.com/powerof3/CommonLibSSE) | **2026-09-01** | Also live, but compile-time version-specific (ifdefs), not multi-runtime. |
| [Ryan-rsm-McKenzie/CommonLibSSE](https://github.com/Ryan-rsm-McKenzie/CommonLibSSE) | 2023-07-20 | The original. Dead. |

## What this project uses (2026-09-09): alandtse/CommonLibSSE-NG v7.5.1, as a submodule

`extern/commonlibsse-ng` is a git submodule of alandtse's `ng` branch at v7.5.1, built with us by `add_subdirectory` (its own tests off); its vcpkg dependencies -- spdlog, rapidcsv, directxtk, fmt -- are in our manifest, since a manifest build reads only the top-level one; the default vcpkg baseline is the one the library's own manifest names. Clone with `--recurse-submodules`, or `git submodule update --init --recursive`: the library carries openvr as a submodule of its own, and the link fails without it.

Things to know about it: `add_commonlibsse_plugin` lives in `cmake/CommonLibSSE.cmake`, which the library's CMakeLists includes only on its prebuilt path, so ours includes it after the subdirectory. There is no `RE::Offset` namespace; a hook target is an address-library id (`RELOCATION_ID(SE, AE)` -- the EquipObject detour uses 37938 / 38894, the pair the library's own wrapper resolves). `Main`'s fields sit behind `GetRuntimeData()`, `ProcessLists::ForEachHighActor` hands out pointers, `GetSlotMask()` is an `EnumSet` (`.underlying()` for the bits), and a perk entry's enums are `BGSEntryPointPerkEntry::Function` and `BGSEntryPointFunctionData::ENTRY_POINT_FUNCTION_DATA`. `SKSE::log::log_directory()` is `My Games\Skyrim Special Edition\SKSE`, beside SKSE's own log. Not modelled: `BGSQuestPerkEntry`. Wrong in the library: `TESPackage::CreatePackage` is declared with `PACKAGE_PROCEDURE_TYPE` (kPackage 46) where the engine takes the record's `PACKAGE_TYPE` (kPackage 18), and given 46 returns a package with no data; `src/game/Forms.cpp` keeps its own correctly typed wrapper (2026-09-09, worth an upstream fix).

## Bug: `ActiveEffect::GetTargetActor` returns a pointer into the middle of the actor (found 2026-09-13)

**What it does.** `ActiveEffect::target` is the engine's `MagicTarget*`, and for an actor that is the `MagicTarget` base inside the `Actor` object, not the object's start. Both overloads of `GetTargetActor` check `target->MagicTargetIsActor()` and then `return reinterpret_cast<Actor*>(target);`, which does not move the pointer. The result is the actor's address plus the base's offset: 0x98 before 1.6.629, 0xA0 from 1.6.629 on, as the library's own forward accessor says (`RUNTIME_CAST_ACCESSOR_VERSIONED(MagicTarget, AsMagicTarget, SKSE::RUNTIME_SSE_1_6_629, 0x98, 0xA0)` in `Actor.h`). Anything read through it -- a field, a virtual call -- reads the `MagicTarget` subobject as if it were the actor.

**How it showed.** Nordic Souls crashed when the Summons tab's Remaining hover (`RemainingBreakdown` in `src/game/Sensors.cpp`) handed `GetTargetActor()`'s result to a perk entry's `CheckConditionFilters` as the Mod Spell Duration target. The engine's check (id 23800) asks each argument `IsBoundObject()`, a TESForm virtual; through the `MagicTarget` vtable that slot is unrelated code, and the game took an access violation in `SkyrimSE.exe+06EE738` (id 39556). The crash log held two pointers both identified as the player, 0xA0 apart: `RDI 0x2350A7D81A0`, the argument, and `0x2350A7D8100` on the stack, the actor. The installed DLL's hash matched the build, so the log was this code.

**Where it came from.** Checked against `C:\modding\CommonLibSSE-NG`, alandtse's `ng` at v8.0.1 (`d13d10a0c`, 2026-09-13), and our submodule at v7.5.1: both have it. From the first version of the file until `197ff1691` (2022-09-22, "Fix variant vtable layout for `Actor` between pre- and post-629 AE releases") the casts were `static_cast`. That commit took `MagicTarget` out of `Actor`'s bases in AE-enabled builds and reached it through `RelocateMember` offsets instead, where `static_cast` no longer compiles; the casts became `reinterpret_cast`, which compiles and drops the offset. `MagicTarget::GetTargetAsActor` (`src/RE/M/MagicTarget.cpp`, added `06ed6387c`, 2024-01-01) is the same `reinterpret_cast<Actor*>(this)` and the same bug.

**Who else it reaches.** Inside the library, SKSE's registration sets key an active effect by its target's form ID: `RegistrationSetUniqueBase::Register` and `Unregister(RE::ActiveEffect*)` (`src/SKSE/RegistrationSetUnique.cpp`) and the three matching calls in `include/SKSE/RegistrationMapUnique.h`. `GetFormID()` is not virtual, so there it does not crash; it reads four bytes of the `MagicTarget` subobject as the form ID, and a registration made for an active effect is filed under that number. Not measured.

**What we do instead.** `effect.target->GetTargetStatsObject()`, the `MagicTarget` virtual the engine itself uses for this (`ActiveEffect::GetVisualsTarget`, and the condition re-check at id 34062), then `As<RE::Actor>()`. It asks the object, so no offset and no runtime are assumed. Our code calls neither `GetTargetActor` nor `GetTargetAsActor`. Not yet seen in play after the change.

**Upstream.** A draft report is in `docs/bugs/commonlibsse-ng-gettargetactor.md`. Whether an issue already exists was not checked: `gh` is not installed here.

## Missing: `ActorEquipManager` declares the equips but neither unequip of a spell or a shout (found 2026-09-17)

**What is missing.** `ActorEquipManager` (`include/RE/A/ActorEquipManager.h`) declares `EquipObject`, `EquipSpell`, `EquipShout` and `UnequipObject` -- and nothing for taking a spell or a shout off. The whole library has no `UnequipSpell` and no `UnequipShout`: a search of the submodule at v7.5.1 for either name returns nothing in `include/` or `src/`. The nearest thing it does declare is `Actor::DeselectSpell` (`RELOCATION_ID(37820, 38769)`), which is a different function with a different job (below).

**Why that is a gap and not a choice.** A plugin that wants to take a spell out of an NPC's hand is left dispatching Papyrus's `Actor.UnequipSpell` through the VM, which runs it on the game thread a frame later. Everything the engine does for equipping is a direct call; only the unequip has to go the long way round, and the lateness is visible: a panel that redraws straight after the call still reads the spell in hand.

**What the engine actually has.** Read off a running 1.6.1170 with `tools/livedisasm.py`, by finding the `"UnequipSpell"` name string (`0x191a600`), the one site that loads it (`0x9f0bb7`, in the Papyrus registration, ID 54784), and the function pointer registered beside it:

| Papyrus native (a wrapper: null-check, then tail-call) | tail-calls | which is |
|---|---|---|
| `Actor.UnequipSpell`, ID 54669 at `0x9e8150` | **ID 38903** at `0x6ca2b0` | `ActorEquipManager::UnequipSpell(Actor*, SpellItem*, source)`: dispatches source 0/1/2 to the hand and voice equip-slot getters (IDs 23607, 23608, 23610), then calls the slot overload, ID 38902 |
| `Actor.UnequipShout`, ID 54664 at `0x9e80b0` | **ID 38904** at `0x6ca320` | `ActorEquipManager::UnequipShout(Actor*, TESShout*)` |

Both wrappers load the equip manager singleton from a global and `jmp` to the method, so the signatures are the manager's own: `(this, actor, spell, source)` and `(this, actor, shout)`. The IDs are in the Address Library already -- what is absent is only the declaration.

**Why `DeselectSpell` is not the same thing.** Read at `0x6c4300`: it walks the four selected-spell slots, nulls any holding the spell and tells that slot's caster, then clears `selectedPower` where what sits there is a `SpellItem` (form type `0x16`). So it takes no hand -- it clears the spell from every slot at once, where `UnequipSpell` takes the source and clears the one asked -- and its form-type gate leaves a shout in the voice alone. For per-hand work it is the wrong tool, and for a shout it does nothing at all.

**What we do instead.** Bind both by ID and call them on the game thread (`UnequipSpellNow` and `UnequipShoutNow`, `src/game/Pins.cpp`), falling back to the Papyrus dispatch where they are not available. AE only for now, since only 1.6.1170 was read; the rows are in `docs/VERSIONS.md`. Not yet verified in play.

**Upstream.** A PR would add the two to `ActorEquipManager.h` and `.cpp` beside `EquipSpell` and `EquipShout`, as `RELOCATION_ID(se, ae)` pairs. The AE halves are 38903 and 38904, read here; the SE halves still need reading against a 1.5.97 binary before either could be offered, which is the same work `docs/VERSIONS.md` already owes for these IDs. Not reported yet, and whether an issue exists was not checked -- `gh` is not installed here.
