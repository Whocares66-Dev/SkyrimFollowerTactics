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
#include "Rule.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ft
{

using ActorId = std::uint32_t; // a FormID, as the game reports it

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
    std::uint8_t hitBy{0};
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

    [[nodiscard]] constexpr bool HitBy(DamageKind kind) const noexcept
    {
        if (kind == DamageKind::Any)
            return hitBy != 0;
        return (hitBy & Bit(kind)) != 0;
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

    // What kind of being the actor is, a bit per TypeKind, set by the game
    // side from the race and the actor's keywords. The Type condition.
    std::uint32_t kinds{0};

    // Of this kind: the kind's own bit, or for a group's head any member's.
    [[nodiscard]] constexpr bool Is(TypeKind kind) const noexcept
    {
        if ((kinds & Bit(kind)) != 0)
            return true;
        return IsGroupHead(kind) && (kinds & GroupBits(kind)) != 0;
    }

    constexpr void SetType(TypeKind kind) noexcept
    {
        kinds |= Bit(kind);
    }
};

// Another actor as a condition reads them: an ally (the player among
// them) or an enemy, the same view either way.
struct ActorView
{
    ActorId id{0};
    Stat health{};
    float distance{0.0f};
    // Whom they are fighting -- an enemy's mark, the player's or a
    // follower's; an ally's target -- or 0 for nobody, the dead included.
    ActorId target{0};
    ActorTraits traits{};
    Stat magicka{};
    Stat stamina{};
    // How far a swing has to reach to strike them, as the engine's melee
    // test measures it (docs/ACTIONS.md 6): centre to centre, less both
    // bodies. What a blow's reach is held against.
    float reachDistance{0.0f};
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
        // A lingering boon: a Fortify, a Resist, a Regenerate -- what a
        // follower is worth buffing with before the swords come out, as
        // against a Restore, which is the emergency being saved for. Judged
        // on the game side from the effect record, not from its name, so a
        // mod's own Fortify counts (src/game/Sensors.cpp, docs/ACTIONS.md).
        // Meaningless on a poison, where every bane goes at the enemy and
        // WantedByAny takes the lot.
        bool buff{false};

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

    // What an "any" action may take of a kind: every bane a poison carries
    // -- they all go at the enemy, so "put something on the blade" needs no
    // further judgement -- but only the lingering boons of anything drunk
    // or eaten. "Drink any potion" would as happily pick the health potion
    // being kept for the emergency, which is the opposite of what a
    // buff-before-the-fight rule is for.
    [[nodiscard]] static constexpr bool WantedByAny(ConsumableKind kind, const Effect &e) noexcept
    {
        return kind == ConsumableKind::Poison || e.buff;
    }

    [[nodiscard]] bool WantedByAny(ConsumableKind kind, const Carried &c) const
    {
        return c.kind == kind && c.count > 0 &&
               std::any_of(c.effects.begin(), c.effects.end(), [&](const Effect &e) { return WantedByAny(kind, e); });
    }

    // One thing of the kind carried, by the snapshot's roll: what an "any"
    // action chooses. Uniform over the FORMS carried and not over the
    // bottles, so twenty of one poison and one of another are equally
    // likely -- which is what makes emptying a bag of odds and ends into a
    // follower work as a tactic rather than as twenty of the commonest.
    // 0 for none carried.
    [[nodiscard]] std::uint32_t AnyForm(ConsumableKind kind, std::uint32_t roll) const
    {
        const auto total = static_cast<std::uint32_t>(
            std::count_if(carried.begin(), carried.end(), [&](const Carried &c) { return WantedByAny(kind, c); }));
        if (total == 0)
            return 0;
        std::uint32_t wanted = roll % total;
        for (const auto &c : carried)
            if (WantedByAny(kind, c) && wanted-- == 0)
                return c.form;
        return 0;
    }

    // One EFFECT of the kind carried, by the same roll: what a Strongest or
    // a Weakest with no effect named chooses by, before choosing the bottle.
    // Effects, not magnitudes, because magnitudes across effects do not
    // compare -- 3 points of Damage Health against 10 of Damage Stamina is
    // not a question with an answer -- so "the strongest poison" has to mean
    // "the strongest of one effect". Distinct names only: two bottles of
    // Damage Health do not make it twice as likely. Empty for none carried.
    [[nodiscard]] std::string AnyEffect(ConsumableKind kind, std::uint32_t roll) const
    {
        std::vector<std::string_view> names;
        for (const auto &c : carried)
        {
            if (c.kind != kind || c.count <= 0)
                continue;
            for (const auto &e : c.effects)
                if (WantedByAny(kind, e) && std::find(names.begin(), names.end(), e.name) == names.end())
                    names.emplace_back(e.name);
        }
        return names.empty() ? std::string{} : std::string(names[roll % names.size()]);
    }

    // Whether every effect an "any" action would have taken this bottle for
    // is already up: the availability a named policy gets from IsRunning,
    // asked of a bottle rather than of an effect.
    [[nodiscard]] bool WantedEffectsRunning(std::uint32_t form, ConsumableKind kind) const
    {
        for (const auto &c : carried)
        {
            if (c.form != form || c.kind != kind)
                continue;
            return std::all_of(c.effects.begin(), c.effects.end(),
                               [&](const Effect &e) { return !WantedByAny(kind, e) || IsRunning(e.name); });
        }
        return false;
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
// The same question PotionStock::running answers for a dose. A buff like
// Oakflesh runs for sixty seconds, far longer than any cooldown worth
// choosing, so spacing cannot solve re-casting and only the effect list
// can: ask whether it is still running.
struct SpellState
{
    std::vector<std::uint32_t> known;
    std::vector<std::uint32_t> active;

    // What each known spell costs THEM, in magicka, with their perks and skill
    // already applied. The game computes it; core only compares it against
    // the magicka they have, so a cast rule they cannot afford is reported as
    // such instead of firing a package the AI will decline.
    struct Cost
    {
        std::uint32_t form{0};
        float magicka{0.0f};
        // Whether they can dual cast it -- the school's Dual Casting perk
        // and a spell whose record leaves a hand free -- and what it costs then:
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
};

struct Snapshot
{
    ActorId self{0};
    double now{0.0}; // seconds, monotonic

    // One random number for this evaluation, drawn on the game side. Every
    // "any" choice indexes with it -- which poison, which effect -- so the
    // engine stays a pure function of its snapshot and a test pins the
    // choice by setting this.
    //
    // Deliberately a value on the snapshot rather than an RNG inside the
    // evaluator: ChosenForm is called more than once per evaluation (once
    // to ask whether the action has what it needs, once to fill the step),
    // and an evaluator that rolled afresh each call would answer those two
    // questions about two different bottles. Fresh each tick, so a rule
    // that fires repeatedly spreads over what is carried.
    std::uint32_t roll{0};

    Stat health{};
    Stat magicka{};
    Stat stamina{};

    bool inCombat{false};
    // A blow the actor could strike with what is in the hands, priced on
    // the game side: whether it is possible at all, the stamina it costs,
    // and how far it reaches, held against an enemy's reachDistance. A
    // blow at an enemy further than that lands on
    // nothing, and the engine charges no stamina for it, so the rule waits
    // for the AI to close. A power attack takes a melee weapon or the
    // fists; a bash takes what blocks: a shield or a torch in the left
    // hand, or the right hand's weapon with the left hand empty.
    struct Blow
    {
        bool possible{false};
        // Whether the follower has what the Settings page requires of this
        // blow: the Power Bash perk, where that is asked for. True when
        // nothing is asked, which is the default and vanilla's own answer.
        bool perk{true};
        float stamina{0.0f};
        float reach{0.0f};
    };
    Blow powerAttack;
    Blow bash;
    Blow powerBash;
    // The blow an action strikes: the power attack for any kind but the
    // two bashes.
    [[nodiscard]] constexpr const Blow &BlowFor(ActionKind kind) const noexcept
    {
        return kind == ActionKind::Bash ? bash : kind == ActionKind::PowerBash ? powerBash : powerAttack;
    }
    [[nodiscard]] constexpr Blow &BlowFor(ActionKind kind) noexcept
    {
        return kind == ActionKind::Bash ? bash : kind == ActionKind::PowerBash ? powerBash : powerAttack;
    }
    // The edges: this is the first evaluation of a fight, or the one
    // farewell evaluation after it. On the farewell pass only CombatEnds
    // holds -- see PredicateKind.
    bool combatBegan{false};
    bool combatEnded{false};
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
        // What a charge would fill: how a Charge policy sizes its gem.
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

    ActorId currentTarget{0};

    // The party and the enemies, by definition (docs/CONDITIONS.md 6): the
    // allies are the player and every other teammate, so the player is
    // read as an ally with kPlayerFormID; the enemies are whoever the
    // compass paints red. Alive ones only.
    std::vector<ActorView> enemies;
    std::vector<ActorView> allies;
    std::vector<CorpseView> corpses;

    [[nodiscard]] const ActorView *Ally(ActorId id) const noexcept
    {
        for (const auto &a : allies)
            if (a.id == id)
                return &a;
        return nullptr;
    }
    [[nodiscard]] const ActorView *Enemy(ActorId id) const noexcept
    {
        for (const auto &e : enemies)
            if (e.id == id)
                return &e;
        return nullptr;
    }

    PotionStock potions;
    SpellState spells;

    // What they could hold or wear, as the pin book describes it -- weapons,
    // shields, torches, armour, ammunition, and the spells they know -- and
    // what is pinned right now, by the panel or by a rule. The equip actions
    // read both: a thing not here cannot be pinned, and one already pinned
    // in the hands asked for is done, so the rule falls through.
    std::vector<Holdable> loadout;
    std::vector<Pin> pins;
};

} // namespace ft
