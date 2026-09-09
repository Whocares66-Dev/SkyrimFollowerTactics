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
