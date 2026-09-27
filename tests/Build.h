#pragma once
// What the tests build snapshots with, shared. No Skyrim here either.

#include "core/Rule.h"
#include "core/Snapshot.h"

#include <algorithm>

namespace ft::test
{

constexpr std::uint32_t kHealthPotion = 0x3EADE;
constexpr std::uint32_t kMagickaPotion = 0x3EAE1;
constexpr std::uint32_t kStaminaPotion = 0x39BE8;

// A follower at full everything, in a fight, with five potions of each of
// the three kinds: the health ones for the rules under test, the other two
// as spare, resource-free actions to tell one rule's firing from another's.
inline Snapshot Healthy()
{
    Snapshot s;
    s.self = 0xA2C94;
    s.now = 100.0;
    s.health = {100.0f, 100.0f};
    s.magicka = {100.0f, 100.0f};
    s.stamina = {100.0f, 100.0f};
    s.inCombat = true;
    s.potions.Add(kHealthPotion, 5, ConsumableKind::Potion, {"Restore Health", 50.0f, 0.0f});
    s.potions.Add(kMagickaPotion, 5, ConsumableKind::Potion, {"Restore Magicka", 50.0f, 0.0f});
    s.potions.Add(kStaminaPotion, 5, ConsumableKind::Potion, {"Restore Stamina", 50.0f, 0.0f});
    return s;
}

// The marquee rule: IF self health below <pct> THEN drink the strongest
// health potion.
inline Rule HealBelow(float pct, const char *label = "heal")
{
    Rule r;
    r.subject = SubjectKind::Self;
    r.predicate = PredicateKind::HealthPctBelow;
    r.conditionArg = pct;
    r.actionTarget = ActionTargetKind::Self;
    r.FirstAction().kind = ActionKind::DrinkStrongest;
    r.FirstAction().effect = "Restore Health";
    r.label = label;
    return r;
}

// The player, as an ally of the snapshot: the one with the player's id,
// added at full everything and close by on the first ask. A Player
// condition reads the player from the allies, as the game builds them
// (dev/CONDITIONS.md 6).
inline ActorView &Player(Snapshot &s)
{
    for (auto &a : s.allies)
        if (a.id == kPlayerFormID)
            return a;
    s.allies.push_back({kPlayerFormID, {100.0f, 100.0f}, 100.0f});
    ActorView &player = s.allies.back();
    player.magicka = player.stamina = {100.0f, 100.0f};
    return player;
}

// A cast a rule names as the game side would judge it (SpellState's casts
// and landings): reaching as `reach` says, and something of it taking on
// `target` or not, singly or dual cast. The cast's reach is set by its first
// call.
inline SpellState::Landing &Lands(Snapshot &s, std::uint32_t form, ActorId target, bool takes,
                                  SpellState::Reach reach = SpellState::Reach::Self, bool dual = false)
{
    if (!s.spells.CastOf(form))
        s.spells.casts.push_back({form, reach});
    s.spells.landings.push_back({form, target, dual, takes});
    return s.spells.landings.back();
}

// The same cast reaching `target` about its centre, where only its area
// effects go: whether any of those would take there. Makes the cast one
// with an area.
inline SpellState::Landing &LandsAbout(Snapshot &s, std::uint32_t form, ActorId target, bool takes, bool dual = false)
{
    auto cast = std::find_if(s.spells.casts.begin(), s.spells.casts.end(),
                             [&](const SpellState::Cast &c) { return c.form == form; });
    if (cast == s.spells.casts.end())
        s.spells.casts.push_back({form, SpellState::Reach::Self, false, true});
    else
        cast->area = true;
    s.spells.landings.push_back({form, target, dual, takes, true});
    return s.spells.landings.back();
}

} // namespace ft::test
