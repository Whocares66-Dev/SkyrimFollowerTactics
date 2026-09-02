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
| [CharmedBaryon/CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) | 2024-09-04 | Stale ~2 years. What the templates and the colorglass vcpkg registry serve. |
| [alandtse/CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG) | **2026-08-30, v7.0.0** | The live NG fork. Multi-runtime, single DLL for SE/AE/VR. |
| [powerof3/CommonLibSSE](https://github.com/powerof3/CommonLibSSE) | **2026-09-01** | Also live, but compile-time version-specific (ifdefs), not multi-runtime. |
| [Ryan-rsm-McKenzie/CommonLibSSE](https://github.com/Ryan-rsm-McKenzie/CommonLibSSE) | 2023-07-20 | The original. Dead. |

## What this project uses, and why

**Currently: CharmedBaryon 3.7.0**, via the colorglass vcpkg registry, baseline bumped from
the template's Nov-2022 pin (which served 3.6.0) to the registry HEAD (which serves 3.7.0).

That is stale, and deliberately so for now. The reasoning:

- CharmedBaryon's last commit is **Sept 2024**, which *postdates* our pinned runtime
  **1.6.1170** (Jan 2024). It knows our target.
- Plugins built with it declare `SKSE::VersionIndependence::AddressLibrary`, so they aren't
  pinned to a runtime list at all — offsets resolve at load time from
  `versionlib-1-6-1170-0.bin`.
- Its staleness only bites for **1.7.x**, which we deliberately deferred to Phase 5.
- Switching build systems before the first successful compile means debugging two unknowns
  at once.

## Known defects in the pinned fork (CharmedBaryon 3.7.0 @ 1.6.1170)

Observed on this machine, not read from an issue tracker:

- **`SKSE::log::log_directory()` returns the wrong folder.** It builds the path from a
  relocated global rather than a literal:

  ```cpp
  path /= *REL::Relocation<const char**>(RELOCATION_ID(508778, 380738)).get();
  ```

  At 1.6.1170 that resolves to `"Skyrim.INI"`, so plugin logs land in
  `My Games\Skyrim.INI\SKSE\` while SKSE64's own log stays in
  `My Games\Skyrim Special Edition\SKSE\`. Cosmetic, but it splits the log directory in
  two and wastes time. **Re-check this first when evaluating the migration** — if
  alandtse v7.0.0 fixes it, that is a small point in favour.

## The upgrade path, when we need it

Move to **alandtse/CommonLibSSE-NG**. Its README documents a submodule rather than a registry:

```sh
git submodule add -b ng https://github.com/alandtse/CommonLibSSE-NG.git lib/commonlibsse-ng
```

then `add_subdirectory(lib/commonlibsse-ng)` in place of `find_package(CommonLibSSE ...)`.
That also drops the colorglass registry entirely, removing a stale third-party dependency.

Take this route when any of these becomes true:

1. The current setup fails to build or misbehaves at runtime.
2. We target 1.7.x (Phase 5). CharmedBaryon cannot; alandtse v7.0.0 explicitly versions the
   AE ≥ 1.7.99 frame fields.
3. We need a fix or type only present upstream.

Note v7.0.0 shipped a breaking change (`State::GetFrameCount()` and friends replacing direct
field access), so it is a real migration, not a version bump.
