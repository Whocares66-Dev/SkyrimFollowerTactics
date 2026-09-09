#pragma once
// Snapshot: everything the rule engine is allowed to know about the world.
//
// HARD RULE: no RE:: types below this line, ever. This header must compile
// with a plain C++23 compiler and no Skyrim, no SKSE, no CommonLibSSE.
// That is the only reason any of this is testable -- there is no headless
// test harness for Skyrim, so the logic has to not need the game.
//
// src/game/ builds one of these per follower per tick. Everything in
// src/core/ consumes it and nothing else.

#include "Kinds.h"
#include "Loadout.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ft
{

using ActorId = std::uint32_t; // FormID, resolved via ResolveFormID on load

// The player is always 0x14. Named here so the evaluator does not carry a
// bare magic number, and so src/game/ and src/core/ agree on it.
inline constexpr ActorId kPlayerFormID = 0x14;

struct Stat
{
    float current{0.0f};
    float max{0.0f};

    [[nodiscard]] constexpr float Pct() const noexcept
    {
        return max > 0.0f ? current / max : 0.0f;
    }
};

// What is true of an actor beyond the numbers, read off the actor each
// tick: the statuses it is in, as one bit each. The same for the follower,
// the player, an ally and an enemy, so a Status condition asks the same
// question of any of them.
struct ActorTraits
{
    std::uint32_t status{0};
    // The share of physical damage the actor's armour turns away, 0 to
    // 0.8: the rating with the hidden per-piece bonus, scaled and capped as
    // the engine does it.
    float armor{0.0f};
    // The resistance to each kind of damage, as the game holds it: percent,
    // negative for a weakness, uncapped. Melee, Ranged and Any stay 0;
    // armour is the physical answer.
    std::array<float, static_cast<std::size_t>(DamageKind::COUNT)> resist{};

    // What has hit the actor in the last few seconds, a bit per DamageKind,
    // and who did it last: the Attacked by condition, and the Attacker
    // target. From the hit table on the game side.
    std::uint8_t attackedBy{0};
    ActorId attacker{0};

    // How many summons and raised corpses the actor commands right now:
    // the engine's commanded-actor list, counted. The Summon condition.
    int summons{0};

    // What is in the actor's hands, a bit per DamageKind: Melee for a
    // blade, Ranged for a bow or crossbow, Magic for a spell or a staff,
    // and the kind of damage any of it does -- an enchantment's, a staff's
    // or a spell's effects, a poison on the blade. The Using condition.
    std::uint8_t wielding{0};

    [[nodiscard]] constexpr bool Using(DamageKind kind) const noexcept
    {
        if (kind == DamageKind::Any)
            return wielding != 0;
        return (wielding & Bit(kind)) != 0;
    }

    constexpr void Wield(DamageKind kind) noexcept
    {
        wielding |= Bit(kind);
    }

    [[nodiscard]] constexpr bool AttackedBy(DamageKind kind) const noexcept
    {
        if (kind == DamageKind::Any)
            return attackedBy != 0;
        return (attackedBy & Bit(kind)) != 0;
    }

    [[nodiscard]] constexpr float Resist(DamageKind kind) const noexcept
    {
        return resist[static_cast<std::size_t>(kind)];
    }

    constexpr void SetResist(DamageKind kind, float value) noexcept
    {
        resist[static_cast<std::size_t>(kind)] = value;
    }

    [[nodiscard]] constexpr bool Has(StatusKind kind) const noexcept
    {
        return (status & Bit(kind)) != 0;
    }

    constexpr void Set(StatusKind kind) noexcept
    {
        status |= Bit(kind);
    }
};

struct EnemyView
{
    ActorId id{0};
    Stat health{};
    float distance{0.0f};
    bool isCasting{false};
    // Whom this enemy is going for: an id, the player's or a follower's,
    // or 0 for nobody in particular.
    ActorId attacking{0};
    bool hasLineOfSight{false};
    ActorTraits traits{};
    // Last, so the tests' positional initialisers above them stand.
    Stat magicka{};
    Stat stamina{};
};

struct AllyView
{
    ActorId id{0};
    Stat health{};
    float distance{0.0f};
    ActorTraits traits{};
    Stat magicka{};
    Stat stamina{};
    // Whom this ally is fighting, or 0.
    ActorId target{0};
};

// A corpse nearby: dead, not already raised or summoned, loaded. Its level
// is what a Reanimate's cap is measured against.
struct CorpseView
{
    ActorId id{0};
    int level{0};
    float distance{0.0f};
};

// What the follower carries to drink, eat or apply, with what each bottle
// does. Populated by an inventory scan in src/game/, which is expensive --
// see the sensor gating note in docs/PLAN.md section 3.3.
struct PotionStock
{
    // One effect of a bottle, by the name the game shows for it, with what
    // the bottle has of it. A potion's boons and a poison's banes: what a
    // Strongest or Weakest policy chooses by. Strength is the magnitude,
    // and among equal magnitudes the duration -- a lingering poison's
    // "1 point for 15 s" over its "1 point for 10 s" -- and for an effect
    // with no magnitude at all (Invisibility, Paralysis) the duration.
    struct Effect
    {
        std::string name;
        float magnitude{0.0f};
        float duration{0.0f};

        [[nodiscard]] bool StrongerThan(const Effect &o) const noexcept
        {
            return magnitude != o.magnitude ? magnitude > o.magnitude : duration > o.duration;
        }
    };

    // Every consumable carried -- potion, food, ingredient, poison -- by
    // form, with its count, kind and effects: what a named consume rule
    // checks against, and what a policy chooses from. Names are display
    // and live on the game side. The kind is checked as well as the form
    // so a hand-edited profile cannot put food under drink-potion: that is
    // a rule that could never work, and the evaluator says so instead of
    // drinking it.
    struct Carried
    {
        std::uint32_t form{0};
        int count{0};
        ConsumableKind kind{ConsumableKind::Potion};
        std::vector<Effect> effects;
    };
    std::vector<Carried> carried;

    // The effects running on the follower right now, by name: a potion
    // whose effect is still up is not drunk again, as a buff is not
    // re-cast.
    //
    // Vanilla alchemy Restore Health is INSTANT -- duration 0, nothing
    // lingers -- so on an unmodded game it is never here and the settle
    // time in MinimumCooldown does the spacing. Potion overhauls commonly
    // convert restores to over-time effects (Potions Restore Over Time,
    // Apothecary, and others), and there a fixed settle is guesswork: the
    // dose might run for ten seconds. Asking the game whether the previous
    // dose is still working is exact, and it costs one walk of the
    // active-effect list we already have. A Fortify or a Resist runs for
    // a minute and is here throughout.
    std::vector<std::string> running;

    [[nodiscard]] int CountOf(std::uint32_t form, ConsumableKind kind) const
    {
        for (const auto &c : carried)
            if (c.form == form && c.kind == kind)
                return c.count;
        return 0;
    }

    [[nodiscard]] bool IsRunning(std::string_view effect) const
    {
        return std::any_of(running.begin(), running.end(), [&](const std::string &r) { return r == effect; });
    }

    // The bottle a policy chooses: of that kind, with that effect, the
    // strongest or the weakest by it. 0 for none carried.
    [[nodiscard]] std::uint32_t Choose(ConsumableKind kind, std::string_view effect, bool strongest) const
    {
        const Carried *best = nullptr;
        const Effect *bestEffect = nullptr;
        for (const auto &c : carried)
        {
            if (c.kind != kind || c.count <= 0)
                continue;
            for (const auto &e : c.effects)
            {
                if (e.name != effect)
                    continue;
                if (!bestEffect || (strongest ? e.StrongerThan(*bestEffect) : bestEffect->StrongerThan(e)))
                {
                    best = &c;
                    bestEffect = &e;
                }
            }
        }
        return best ? best->form : 0;
    }

    // Add a bottle, or one effect to a bottle already listed.
    void Add(std::uint32_t form, int count, ConsumableKind kind, const Effect &effect)
    {
        for (auto &c : carried)
        {
            if (c.form == form && c.kind == kind)
            {
                c.effects.push_back(effect);
                return;
            }
        }
        carried.push_back({form, count, kind, {effect}});
    }
};

// Spells the follower knows, and the ones whose effects are running right now.
//
// Both are FormIDs and both are opaque to core -- it never resolves them, it
// only asks whether one is in a list. That keeps the "is this buff already up"
// question answerable without core knowing what a spell is.
//
// This is the general form of what PotionStock's three bools do for restores.
// A buff like Oakflesh runs for sixty seconds, far longer than any cooldown
// worth choosing, so spacing cannot solve re-casting and only the effect list
// can: ask whether it is still running.
struct SpellState
{
    std::vector<std::uint32_t> known;
    std::vector<std::uint32_t> active;
    std::vector<std::uint32_t> equipped;

    // What each known spell costs HER, in magicka, with her perks and skill
    // already applied. The game computes it; core only compares it against
    // the magicka she has, so a cast rule she cannot afford is reported as
    // such instead of firing a package the AI will decline.
    struct Cost
    {
        std::uint32_t form{0};
        float magicka{0.0f};
        // Whether she can dual cast it -- the school's Dual Casting perk
        // and a spell that leaves a hand free -- and what it costs then:
        // the cost times the game's dual-casting multiplier, unless the
        // spell is flagged to take no dual-cast change.
        bool dualable{false};
        float dualMagicka{0.0f};
    };
    std::vector<Cost> costs;

    [[nodiscard]] bool CanDualCast(std::uint32_t form) const
    {
        for (const auto &c : costs)
            if (c.form == form)
                return c.dualable;
        return false;
    }

    [[nodiscard]] float DualCostOf(std::uint32_t form) const
    {
        for (const auto &c : costs)
            if (c.form == form)
                return c.dualMagicka;
        return 0.0f;
    }

    // Zero for a spell with no recorded cost, so a snapshot that does not
    // carry costs (a test, an older sensor) never blocks a cast.
    [[nodiscard]] float CostOf(std::uint32_t form) const
    {
        for (const auto &c : costs)
            if (c.form == form)
                return c.magicka;
        return 0.0f;
    }

    // The level cap of a Reanimate spell: the highest level of corpse it
    // raises, read off its effect's magnitude on the game side. Absent for
    // every other spell. The Corpse subject filters by the rule's spell.
    struct Cap
    {
        std::uint32_t form{0};
        int maxLevel{0};
    };
    std::vector<Cap> caps;

    [[nodiscard]] int CapOf(std::uint32_t form) const
    {
        for (const auto &c : caps)
            if (c.form == form)
                return c.maxLevel;
        return 0;
    }

    [[nodiscard]] bool Knows(std::uint32_t form) const
    {
        return std::find(known.begin(), known.end(), form) != known.end();
    }

    [[nodiscard]] bool IsActive(std::uint32_t form) const
    {
        return std::find(active.begin(), active.end(), form) != active.end();
    }

    [[nodiscard]] bool IsEquipped(std::uint32_t form) const
    {
        return std::find(equipped.begin(), equipped.end(), form) != equipped.end();
    }
};

struct Snapshot
{
    ActorId self{0};
    double now{0.0}; // seconds, monotonic

    Stat health{};
    Stat magicka{};
    Stat stamina{};

    bool inCombat{false};
    // The edges: this is the first evaluation of a fight, or the one
    // farewell evaluation after it. On the farewell pass only CombatEnds
    // holds -- see PredicateKind.
    bool combatBegan{false};
    bool combatEnded{false};
    bool weaponDrawn{false};
    bool sneaking{false};
    // Each hand's weapon: whether it takes a poison (anything but a staff;
    // false with no weapon there) and whether it carries one. The Weapon
    // poison condition reads both; an Apply rule needs a hand that takes
    // one and is clean, the right before the left.
    struct HandWeapon
    {
        bool takesPoison{false};
        bool poisoned{false};
        // Its enchantment's charge, when it has one: what is left, the
        // full amount, and what one hit draws. Needed when a hit cannot be
        // paid for, which is when the enchantment stops landing.
        bool enchanted{false};
        float charge{0.0f};
        float maxCharge{0.0f};
        float costPerHit{0.0f};
        [[nodiscard]] constexpr bool Clean() const noexcept
        {
            return takesPoison && !poisoned;
        }
        [[nodiscard]] constexpr bool ChargeNeeded() const noexcept
        {
            return enchanted && maxCharge > 0.0f && charge < costPerHit;
        }
        [[nodiscard]] constexpr float Missing() const noexcept
        {
            return enchanted ? maxCharge - charge : 0.0f;
        }
    };
    HandWeapon rightWeapon;
    HandWeapon leftWeapon;
    [[nodiscard]] constexpr bool AnyWeaponTakesPoison() const noexcept
    {
        return rightWeapon.takesPoison || leftWeapon.takesPoison;
    }
    [[nodiscard]] constexpr bool AnyWeaponClean() const noexcept
    {
        return rightWeapon.Clean() || leftWeapon.Clean();
    }
    [[nodiscard]] constexpr bool AnyWeaponPoisoned() const noexcept
    {
        return rightWeapon.poisoned || leftWeapon.poisoned;
    }
    [[nodiscard]] constexpr bool AnyWeaponEnchanted() const noexcept
    {
        return rightWeapon.enchanted || leftWeapon.enchanted;
    }
    [[nodiscard]] constexpr bool AnyWeaponChargeNeeded() const noexcept
    {
        return rightWeapon.ChargeNeeded() || leftWeapon.ChargeNeeded();
    }

    // The filled soul gems carried, each with what its soul puts into a
    // charge. A reusable one is emptied when spent, not lost.
    struct SoulGemView
    {
        std::uint32_t form{0};
        int count{0};
        float charge{0.0f};
    };
    std::vector<SoulGemView> soulGems;
    // Seconds until the voice can shout again, 0 when it can. The engine
    // keeps this per actor -- NPCs too -- as a shout's word recovery time
    // set when the shout fires, and a Shout rule inside it reports
    // Recovering rather than firing into a shout the AI will not make.
    float voiceRecovery{0.0f};
    ActorTraits traits{};

    Stat playerHealth{};
    Stat playerMagicka{};
    Stat playerStamina{};
    ActorTraits playerTraits{};

    ActorId currentTarget{0};
    // Whom the player is fighting, for "target of the player": focus fire
    // is the enemy the player has picked.
    ActorId playerTarget{0};

    std::vector<EnemyView> enemies;
    std::vector<AllyView> allies;
    std::vector<CorpseView> corpses;

    PotionStock potions;
    SpellState spells;

    // What she could hold or wear, as the pin book describes it -- weapons,
    // shields, torches, armour, ammunition, and the spells she knows -- and
    // what is pinned right now, by the panel or by a rule. The equip actions
    // read both: a thing not here cannot be pinned, and one already pinned
    // in the hands asked for is done, so the rule falls through.
    std::vector<Holdable> loadout;
    std::vector<Pin> pins;
};

} // namespace ft
