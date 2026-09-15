#pragma once
// Forms made in memory at load, so this mod ships no plugin file.
//
// WHY
// Everything the cast machinery needs from a record -- packages, wrapper
// shouts, words -- is content only in the sense that the engine wants it in a
// form. Nothing about it is authored per player or per
// follower, and none of it belongs in a save: a follower's rules go through
// the co-save, the packages are a mechanism. Making them at load means the
// load order does not change when the mod is installed, nothing of ours is
// referenced from the save file, and removing the DLL removes the whole mod.
//
// HOW, AND WHAT WAS CHECKED
// Read from the 1.6.1170 executable (tools/disasm.py, docs/MAGIC.md "Forms at
// runtime") rather than assumed:
// - TESForm's constructor gives a form made outside file loading a dynamic
//   FormID from the data handler and registers it in the global form map.
//   The allocator (13740) counts up from FF000800, skips an ID the form map
//   or the loaded save (BGSSaveLoadGame::IsFormIDInUse) holds, and wraps at
//   FF3FFFFF. Its counter is restored from the save on load, and a save
//   brings its own created objects (potions, enchantments) back at their
//   saved IDs without asking, so a form of ours made before a load could sit
//   where one of them lands. Ours are moved to IDs from kFirstFormId up,
//   which a save's counter reaches only after some four million created
//   forms, each checked as the allocator checks.
// - TESForm::Copy and CreateDuplicateForm copy NOTHING for a package: the
//   package does not override them. The engine's own copy of a package's
//   inputs is TESCustomPackageData::Copy, which recreates every input through
//   the type registry and assigns it from the source, each target input with
//   its own PackageTarget. That is what ClonePackage calls, and the
//   package-level fields (flags, conditions) are set here by hand.
// - TESCustomPackageData::InitItem links a package to its template and shares
//   the template's procedure tree and name map; it creates no inputs. So a
//   runtime package must get its inputs by copying from a finished instance,
//   not from a template link alone.
// - The engine serializes a runtime-made package if an actor is running it
//   when the game saves, and rebuilds a hollow one on load. Packages.cpp
//   releases every lease on the save message, before the write, so that
//   never happens.

#include <cstdint>

namespace RE
{
struct TESConditionItem;
class TESForm;
class TESPackage;
class TESShout;
class TESWordOfPower;
} // namespace RE

namespace ft::game
{

// Where the walk for our IDs starts, and the last ID a dynamic form can have.
// An ID another plugin's form already holds is skipped, so it costs an ID
// rather than the feature.
inline constexpr std::uint32_t kFirstFormId = 0xFF3F0800;
inline constexpr std::uint32_t kLastFormId = 0xFF3FFFFF;

// Whether this file made the form with this ID. Not the same as an ID in the
// range above: one the walk skipped belongs to someone else. Game thread only,
// as the forms are made there.
[[nodiscard]] bool MadeByUs(std::uint32_t formId);

// A package with the same inputs as `source`, its own copy of each, at the
// next free ID. packData is set to what the ESP-era records carried:
// IgnoreCombat, Run, no interrupt override, no interrupt flags. No
// conditions; add one with AddIsReferenceCondition. Null if the engine
// refused.
[[nodiscard]] RE::TESPackage *ClonePackage(RE::TESPackage *source);

// Give a package a single condition, `GetIsReference(<none>) == 1` on the
// subject, and return the item so the caller can point its parameter at an
// actor. The engine's GetIsReference is a null-safe pointer compare on a
// reference-typed parameter, so with the parameter null the condition fails
// and with it set to an actor it passes for that actor alone.
[[nodiscard]] RE::TESConditionItem *AddIsReferenceCondition(RE::TESPackage *pkg);

// A word of power and a one-word wrapper shout, as the ESP-era records were:
// the word is a label; the shout's first variation is the word with the
// given spell and a one-second recovery, the other two empty.
[[nodiscard]] RE::TESWordOfPower *CreateWord(const char *name);
[[nodiscard]] RE::TESShout *CreateShout(RE::TESWordOfPower *word, RE::TESForm *spell, const char *name);

} // namespace ft::game
