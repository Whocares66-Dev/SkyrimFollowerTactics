#pragma once

// Every address the plugin takes from the game's executable, in one place:
// what CommonLib does not map, as the Address Library ID pair (Special
// Edition, Anniversary Edition) and the vtable slots read by hand. The SE
// and AE databases are separately numbered, so each pair is two different
// numbers for one function. dev/VERSIONS.md is the row for each: what it
// is, what uses it, how it was found on each line and how far it was
// checked. A new one gets a row there and a name here; nothing names a
// bare REL::ID anywhere else.
//
// Read on 1.6.1170 (AE) and 1.5.97 (SE) with tools/disasm.py, matched
// across the line by shape and by callers.

#include "RE/Skyrim.h"
#include "REL/Relocation.h"

namespace ft::game::addr
{

// TESPackage::CreatePackage(PACKAGE_TYPE): the engine's own factory for a
// runtime package record (dev/MAGIC.md "Forms at runtime").
inline constexpr REL::RelocationID kCreatePackage{28732, 29496};

// ActorEquipManager's three equip entries, detoured so the engine's own
// equips are refused against the pins and bans (Pins.cpp).
inline constexpr REL::RelocationID kEquipObject{37938, 38894};
inline constexpr REL::RelocationID kEquipSpell{37939, 38895};
inline constexpr REL::RelocationID kEquipShout{37941, 38897};

// ActorEquipManager::UnequipSpell(actor, spell, source) and
// UnequipShout(actor, shout), which CommonLib declares for neither: what
// the Papyrus natives Actor.UnequipSpell and Actor.UnequipShout tail-call
// after their null check (dev/COMMONLIB.md). Source 0 is the left hand, 1
// the right, 2 the voice.
inline constexpr REL::RelocationID kUnequipSpell{37947, 38903};
inline constexpr REL::RelocationID kUnequipShout{37948, 38904};

// Turn an actor toward a point through its movement controller, and take
// the point back: the UseWeapon procedure's own calls out of combat
// (dev/ATTACK.md "Facing"). (actor, point, tolerance in radians, 1, 1).
inline constexpr REL::RelocationID kTurnToward{36818, 37834};
inline constexpr REL::RelocationID kStopTurning{36823, 37839};

// The global map from a quest alias to its BGSOverridePackCollection: this
// is its capacity field, 0x0C into a BSTScatterTable whose layout
// Packages.cpp copies (CommonLib maps the container but not this global).
inline constexpr REL::RelocationID kAliasOverrideMapCapacity{502247, 369298};

// CombatInventoryItem's score call: the slot replaced in each entry
// class's vtable so the AI's choice defers to the pins (dev/UNIQUE.md).
inline constexpr std::size_t kCalculateScoreSlot = 0x0C;

} // namespace ft::game::addr
