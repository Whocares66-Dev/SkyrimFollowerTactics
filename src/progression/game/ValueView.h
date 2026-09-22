#pragma once
// A companion's skills and attributes as the engine reads them: what the
// engine gives them, plus what they have learned and the points assigned --
// without writing either to the actor, so the save holds none of it and the
// game without Progression, or with progression off, has them as they were
// (dev/ENGINE_SKILLS.md, "Read, not written").
//
// One engine function is replaced, and passes straight through for every
// actor that is not a managed companion:
//
//   GetBaseActorValue   Character's, slot 3 of its ActorValueOwner part.
//                       The permanent value (38484 on AE, 37535 on SE)
//                       and the current one (38462 on AE) read the base
//                       through that slot, so a skill or an attribute read
//                       any of the three ways has what they learned in it.
//
// Game thread publishes; the hook runs on any thread.

#include "progression/core/Skills.h"

#include <unordered_map>

namespace fp::game::valueview
{

// What a managed companion's reads are to add: learned levels per skill,
// and each attribute's assigned amount in the engine's units.
struct Bonus
{
    PerSkill<int> skills{};
    PerAttribute<int> attributes{};
};

// At data load. Safe with nobody managed.
void Install();
[[nodiscard]] bool Installed() noexcept;

// Game thread: every managed actor's bonus, by the reference's runtime id,
// replacing the last whole; `skillCap` the skill maximum reads are held to.
// An actor not in it reads as the engine has them.
void Publish(std::unordered_map<RE::FormID, Bonus> bonuses, int skillCap);

// What the engine gives them, without what they learned: for Progression's
// own reckoning, which adds the ledger itself.
[[nodiscard]] float EngineBase(RE::Actor *actor, RE::ActorValue av);

// Game thread, before a load or a new game: nobody's.
void Forget();

} // namespace fp::game::valueview
