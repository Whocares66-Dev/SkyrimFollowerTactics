# Draft: `ActiveEffect::GetTargetActor()` and `MagicTarget::GetTargetAsActor()` return a pointer offset into the actor

For alandtse/CommonLibSSE-NG. Not filed. Before filing: search the repo's issues and PRs for `GetTargetActor` / `GetTargetAsActor` (not done here, no `gh`), and confirm against the then-current `ng`.

---

**Title:** `ActiveEffect::GetTargetActor()` / `MagicTarget::GetTargetAsActor()` reinterpret_cast a `MagicTarget*` to `Actor*` without the base offset

## Summary

`ActiveEffect::target` is a `MagicTarget*`. For an actor it points at the `MagicTarget` base inside the `Actor` object (offset 0x98 before 1.6.629, 0xA0 from 1.6.629 on). `ActiveEffect::GetTargetActor()` and `MagicTarget::GetTargetAsActor()` convert it with `reinterpret_cast<Actor*>`, which does not subtract that offset, so they return `actor + 0x98` / `actor + 0xA0` instead of the actor. Fields and virtuals read through the result read the `MagicTarget` subobject.

## Affected

- `src/RE/A/ActiveEffect.cpp`, both overloads of `GetTargetActor()`:
  ```cpp
  if (target && target->MagicTargetIsActor()) {
      return reinterpret_cast<Actor*>(target);
  }
  ```
- `src/RE/M/MagicTarget.cpp`, `MagicTarget::GetTargetAsActor()`:
  ```cpp
  if (MagicTargetIsActor()) {
      return reinterpret_cast<Actor*>(this);
  }
  ```
- Seen at `ng` v8.0.1 (`d13d10a0c`) and v7.5.1.
- History: the `GetTargetActor` casts were `static_cast` until `197ff1691` ("Fix variant vtable layout for `Actor` between pre- and post-629 AE releases", 2022-09-22), which made `MagicTarget` a runtime-offset base (`AsMagicTarget()` through `RelocateMember`); `static_cast` no longer compiled there and `reinterpret_cast` replaced it. `GetTargetAsActor` was added with `reinterpret_cast` in `06ed6387c` (2024-01-01).

## Impact

- Any caller that uses the result as an actor. A virtual call through it dispatches through the `MagicTarget` vtable and can crash.
- Inside the library: `RegistrationSetUniqueBase::Register` / `Unregister(RE::ActiveEffect*)` (`src/SKSE/RegistrationSetUnique.cpp`) and the matching calls in `include/SKSE/RegistrationMapUnique.h` key registrations by `GetTargetActor()->GetFormID()`. `GetFormID()` is not virtual, so this does not crash; it reads bytes of the `MagicTarget` subobject as the form ID. (Inferred from the code, not measured.)

## How it was found

Skyrim SE 1.6.1170, SKSE 2.2.8, CommonLibSSE-NG v7.5.1 built for SE+AE+VR, a heavily modded load order (PerkEntryPointExtender present). A plugin passed `effect->GetTargetActor()` as the target argument to `BGSEntryPointPerkEntry::CheckConditionFilters` for Mod Spell Duration. The engine's condition check calls `IsBoundObject()` (TESForm vtable slot 0x27) on each argument; through the `MagicTarget` vtable that slot is unrelated code, and the game crashed:

```
Unhandled exception "EXCEPTION_ACCESS_VIOLATION" at 0x7FF75FFAE738 SkyrimSE.exe+06EE738	movsd [rdx], xmm0
PROBABLE CALL STACK:
	[0] 0x7FF75FFAE738 SkyrimSE.exe+06EE738 -> 39556+0x18
REGISTERS:
	RDI 0x2350A7D81A0      (PlayerCharacter*)      <- the argument: GetTargetActor()'s result
	R14 0x2352A7C1930      (BGSEntryPointPerkEntry*)
	R15 0x3                                        <- argument count
STACK:
	[RSP+30  ] SkyrimSE.exe+038F115   test al, al  <- just after the IsBoundObject() virtual call in the condition check (id 23800)
	[RSP+130 ] 0x2350A7D8100      (PlayerCharacter*)  <- the actor itself: 0xA0 below the argument
```

Replacing the call with `target->GetTargetStatsObject()` and `As<RE::Actor>()` removed the crash path.

## Suggested fix

Either of:

1. Ask the object, as the engine does (`ActiveEffect::GetVisualsTarget` is `target ? target->GetTargetStatsObject() : 0`), which assumes no offset or runtime:
   ```cpp
   Actor* ActiveEffect::GetTargetActor()
   {
       const auto ref = target ? target->GetTargetStatsObject() : nullptr;
       return ref ? ref->As<Actor>() : nullptr;
   }
   ```
2. Subtract the same runtime-chosen offset `Actor::AsMagicTarget()` adds (`RUNTIME_CAST_ACCESSOR_VERSIONED(MagicTarget, AsMagicTarget, SKSE::RUNTIME_SSE_1_6_629, 0x98, 0xA0)`), e.g. an inverse of `RelocateMemberIfNewer`, in both `GetTargetActor` overloads and `MagicTarget::GetTargetAsActor`.

A regression check: for any active effect on an actor, `effect->GetTargetActor()` should equal `effect->target->GetTargetStatsObject()`, and `actor->AsMagicTarget()->GetTargetAsActor()` should equal `actor`.

## Not verified

- Behaviour on VR (the forward accessor's VR offset was not checked).
- The registration-set impact in a running game.
- Whether an issue or PR already exists.
