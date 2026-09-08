#pragma once
// Forms made in memory at load, so this mod ships no plugin file.
//
// WHY
// Everything the cast machinery needs from a record -- sixteen packages, eight
// wrapper shouts, eight words -- is content only in the sense that the engine
// wants it in a form. Nothing about it is authored per player or per
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
//   Those IDs are FF000800..FF3FFFFF, and the counter is restored from the
//   save on load, so a save's own created objects (potions, enchantments)
//   can sit at the same IDs a plugin took at start-up. Ours are moved to a
//   range the allocator never reaches (kRuntimeFormBase) so neither side can
//   collide with the other.
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

// Where our forms live. The engine's dynamic-ID allocator wraps at FF3FFFFF
// and a save's created-object references are 22 bits, so FF3F0000.. is
// reachable by neither; a form here can still be looked up by ID and, should
// a reference ever leak into a save, round-trips to a unique ID that simply
// resolves to nothing in a session without the mod. The low bits are the
// mod's own local IDs, kept for the log.
inline constexpr std::uint32_t kRuntimeFormBase = 0xFF3F0000;

// A package with the same inputs as `source`, its own copy of each, at our
// ID. packData is set to what the ESP-era records carried: IgnoreCombat, Run,
// no interrupt override, no interrupt flags. No conditions; add one with
// AddIsReferenceCondition. Null if the engine refused.
[[nodiscard]] RE::TESPackage *ClonePackage(RE::TESPackage *source, std::uint32_t localID);

// Give a package a single condition, `GetIsReference(<none>) == 1` on the
// subject, and return the item so the caller can point its parameter at an
// actor. The engine's GetIsReference is a null-safe pointer compare on a
// reference-typed parameter, so with the parameter null the condition fails
// and with it set to an actor it passes for that actor alone.
[[nodiscard]] RE::TESConditionItem *AddIsReferenceCondition(RE::TESPackage *pkg);

// A word of power and a one-word wrapper shout, as the ESP-era records were:
// the word is a label; the shout's first variation is the word with the
// given spell and a one-second recovery, the other two empty.
[[nodiscard]] RE::TESWordOfPower *CreateWord(std::uint32_t localID, const char *name);
[[nodiscard]] RE::TESShout *CreateShout(std::uint32_t localID, RE::TESWordOfPower *word, RE::TESForm *spell,
                                        const char *name);

} // namespace ft::game
