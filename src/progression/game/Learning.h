#pragma once
// Where a companion's skill use comes from (dev/ENGINE_SKILLS.md). The engine
// works out a skill use for whoever acts, and for everyone but the player
// either throws it away or never works it out:
//
//   Character's UseSkill   every spell's use goes through it, for any caster;
//                          an NPC's is a bare `ret`. Replaced, so a
//                          companion's comes to us, with the engine's points.
//   the hit handler        the weapon, Block and armour uses are worked out
//                          there for the player only. Its call handing the
//                          finished HitData to the victim's processing is
//                          hooked, and the same uses worked out, the same
//                          way, for a companion attacking or hit.
//
// Every use found is queued to the game thread and handed to
// progression/game/Service.h's OnSkillUse; nothing here changes the actor.

#include <cstdint>
#include <vector>

namespace fp::game::learning
{

// At data load. Not on VR.
void Install();
[[nodiscard]] bool Installed() noexcept;

// Game thread: the actors whose skill use is counted, by runtime id.
// Empty: nobody's.
void Publish(const std::vector<RE::FormID> &actors);

// Before a load or a new game: nobody's, and a use still queued from the
// game being left is dropped rather than credited to one of the same id.
void Forget();

// Uses heard so far, by where they came from: the first thing to look at in
// play.
struct Counters
{
    std::uint64_t magic{0};  // through UseSkill
    std::uint64_t blows{0};  // a companion's blow landing
    std::uint64_t struck{0}; // a companion hit: Block or armour
};
[[nodiscard]] Counters Count() noexcept;

} // namespace fp::game::learning
