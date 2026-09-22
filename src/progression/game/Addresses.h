#pragma once

// Every address the plugin takes from the executable that CommonLib does not
// map, as an Address Library ID pair (Special Edition, Anniversary Edition),
// and the vtable slots patched by hand, as src/game/Addresses.h is for
// Tactics. dev/ENGINE_PERKS.md, dev/ENGINE_SPELLS.md and dev/ENGINE_SKILLS.md
// have how each was found and on which builds it was read: 1.5.97, 1.6.1170
// and 1.7.104, with tools/disasm.py; dev/VERSIONS.md has a row for each.

#include "REL/Relocation.h"

#include <cstddef>

namespace fp::game::addr
{

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

// The melee and projectile hit handler, and inside it the call that hands
// the finished HitData to the victim's processing (38586 on AE, 37633 on
// SE): the call hit mods commonly hook. The weapon, Block and armour skill
// uses it makes just before are the player's alone.
inline constexpr REL::RelocationID kHitHandler{37673, 38627};
inline constexpr REL::VariantOffset kHitHandlerVictimCall{0x3C0, 0x4A8, 0};

} // namespace fp::game::addr
