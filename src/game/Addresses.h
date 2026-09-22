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
#include "SKSE/Version.h"

#include <cstddef>

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

// BSInputDeviceManager's per-frame hand-off of the input queue to its
// sinks, and the offset inside it of the one call that does the handing:
// the call SKSE Menu Framework rewrites to read the keys, and that the
// panel rewrites after it so Escape can be kept from closing the menu
// (UI.cpp, dev/VERSIONS.md).
inline constexpr REL::RelocationID kInputQueueDispatch{67315, 68617};
inline constexpr std::ptrdiff_t kInputQueueDispatchCall = 0x7B;

// The global map from a quest alias to its BGSOverridePackCollection: this
// is its capacity field, 0x0C into a BSTScatterTable whose layout
// Packages.cpp copies (CommonLib maps the container but not this global).
inline constexpr REL::RelocationID kAliasOverrideMapCapacity{502247, 369298};

// CombatInventoryItem's score call: the slot replaced in each entry
// class's vtable so the AI's choice defers to the pins (dev/UNIQUE.md).
inline constexpr std::size_t kCalculateScoreSlot = 0x0C;

// --- Progression: the perk and spell views, and learning by doing -----------
// (dev/ENGINE_PERKS.md, dev/ENGINE_SPELLS.md, dev/ENGINE_SKILLS.md, read on
// 1.5.97, 1.6.1170 and 1.7.104)

// TaskQueueInterface's queued perk rank change for any actor:
// (queue, actor, perk, old rank, new rank). What Character's own
// ApplyPerksFromBase calls for each perk on the base record (0 -> its rank),
// and what the player's AddPerk calls (0 -> rank). Read on both lines; the SE
// body is an exact shape match of the AE one (tools/disasm.py --match).
inline constexpr REL::RelocationID kQueuePerkRankChange{36007, 36982};

// Character's vtable slots (the same numbers on SE and AE; VR differs and is
// not built). The engine's HasPerk for any actor is ForEachPerk with a finder
// visitor, so patching ForEachPerk changes HasPerk too.
inline constexpr std::size_t kForEachPerkSlot = 0xFA;
inline constexpr std::size_t kApplyPerksFromBaseSlot = 0x101;

// Actor::VisitSpells(visitor): every spell an actor knows -- added to them,
// on their record, their race's, their record's leveled lists as resolved in
// their process -- each handed to the visitor's slot 1, which answers 1 to go
// on. HasSpell is this walk with a finder, and the combat AI's inventory is
// gathered through it. Not virtual, so detoured (Detours). CommonLib maps it
// as Actor::VisitSpells with the same pair.
inline constexpr REL::RelocationID kVisitSpells{37827, 38781};

// Character's CheckCast slot: can the actor cast this now, asked of each of
// its four magic casters. What the UseMagic package procedure (38727 on AE)
// asks of every spell on the record before choosing one.
inline constexpr std::size_t kCheckCastSlot = 0x110;

// --- learning by doing (dev/ENGINE_SKILLS.md) ---------------------------------

// Actor's UseSkill slot: (actor, skill, points, form, usage). Every NPC's is
// a bare `ret` (37647 on AE); the player's (40488) turns the points into
// skill XP. The engine calls it for the caster of a spell, whoever that is.
inline constexpr std::size_t kUseSkillSlot = 0xF7;

// A skill's usage values from its record: (skill, &useMult, &useOffset,
// &improveMult, &improveOffset) -> bool. What the player's skill advance
// (41561) reads. The SE body is an exact match.
inline constexpr REL::RelocationID kSkillUsage{26576, 27244};

// A bash's skill points: (total damage, has a shield) -> points, from
// f{Shield,Weapon}BashSkillUse{Mult,Base}. Pure; the hit handler's.
inline constexpr REL::RelocationID kBashSkillUse{25857, 26423};

// The shield an actor has equipped: (actor) -> TESObjectARMO*, or none.
// Exact SE match.
inline constexpr REL::RelocationID kEquippedShield{37624, 38577};

// Character's ActorValueOwner part: its table is the sixth of Character's
// (index 5 of VTABLE_Character; SE 261402, AE 207896), and its slot 3 is
// GetBaseActorValue (37519 on SE, 38464 on AE). The permanent value (37535 on
// SE, 38484 on AE) and the current one (38462 on AE) call the base through
// that slot, so the value view is one slot (progression/game/ValueView.h).
// The part sits 0xB0 into the actor before 1.6.629 and 0xB8 from it, as
// CommonLib's Actor::AsActorValueOwner has it and the executables' RTTI
// says (1.5.97, 1.6.1170, 1.7.104).
inline constexpr std::size_t kCharacterValueOwnerTable = 5;
inline constexpr std::size_t kGetBaseActorValueSlot = 3;
[[nodiscard]] inline std::ptrdiff_t ActorValueOwnerOffset() noexcept
{
    return REL::Module::get().version() < SKSE::RUNTIME_SSE_1_6_629 ? 0xB0 : 0xB8;
}

// (actor, value): the value's current and maximum marked stale in the
// cache on the actor's process, for the values whose info says the process
// caches them (kAIProcessCachesCurrentValue, kAIProcessCachesMaxValue) --
// the skills among them. The engine's own base setter calls it after every
// write (38465 on AE, through 37561 on SE); a value read with the cache
// fresh is the cache's, whatever the base now reads. Same ID on 1.7.104
// (an exact match); SE's by its place in the setter and its shape.
inline constexpr REL::RelocationID kMarkValueStale{37534, 38483};

// The melee and projectile hit handler, and inside it the call that hands
// the finished HitData to the victim's processing (38586 on AE, 37633 on
// SE): the call hit mods commonly hook. The weapon, Block and armour skill
// uses it makes just before are the player's alone.
inline constexpr REL::RelocationID kHitHandler{37673, 38627};
inline constexpr REL::VariantOffset kHitHandlerVictimCall{0x3C0, 0x4A8, 0};

} // namespace ft::game::addr

// Progression's code, in fp::game, names these as addr:: too.
namespace fp::game
{
namespace addr = ft::game::addr;
}
