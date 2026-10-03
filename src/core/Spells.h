#pragma once
// Which of an actor's spells, shouts, powers, staves and scrolls the snapshot
// calls known, which powers are used today, which staves are spent, which
// spells are castable
// and so described and priced, and which spells and shouts are active by
// the effects running. The game reads the records and the effect list
// (game/Sensors.cpp, BuildSnapshot) and hands the facts here; the sorting
// is decided here, where it is tested. No Skyrim.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ft
{

// One thing the actor has to cast, as read.
struct SpellSeen
{
    std::uint32_t id{0};
    enum class Kind : std::uint8_t
    {
        Shout,
        Staff,
        Scroll,
        Power,
        Spell
    };
    Kind kind{Kind::Spell};
    bool wrapper{false};   // a shout: one of ours, made for a power lease
    int highestWord{-1};   // a shout: the highest word unlocked, -1 for none
    int carried{1};        // a scroll or a staff: how many
    bool charged{true};    // a staff: a copy of it can pay for a cast
    bool castable{true};   // a spell: one the AI can cast (IsCastable)
    bool greater{false};   // a power: a greater power, once a day
    bool usedToday{false}; // a greater power: on the engine's used list
};

struct SpellsKnown
{
    std::vector<std::uint32_t> known;     // in the order seen
    std::vector<std::uint32_t> usedToday; // greater powers used today
    std::vector<std::uint32_t> spent;     // staves carried with no copy that can pay for a cast
    std::vector<std::uint32_t> castable;  // the spells for the loadout and the pricing
};

// A shout is known with a word unlocked and not a wrapper: a rule that
// names one with no word reports it missing rather than firing into
// silence. A scroll is known while carried: knowing and carrying are the
// one question for it. A staff likewise, and one out of charge is said so,
// as a greater power used today is: carried all the same, so its rule
// reads as waiting on a soul gem rather than as naming nothing. A power is
// known, and a greater one used today is said so. A spell is known when castable, and is then the loadout's and
// priced.
[[nodiscard]] SpellsKnown ClassifySpells(std::span<const SpellSeen> seen);

// One copy of a staff carried, as read: in a hand or in the bag, and the
// charge it holds -- the hand's for one in hand, where the engine keeps
// the live charge, and the copy's own in the bag.
struct StaffCopy
{
    bool inHand{false};
    float charge{0.0f};
};

// The copy a cast takes, of those that can pay for one: one already in a
// hand, which needs no equip, else the fullest in the bag, the first of
// equals. None when no copy can pay, which is a staff spent.
[[nodiscard]] std::optional<std::size_t> StaffCopyToCast(std::span<const StaffCopy> copies, float cost) noexcept;

// One effect running on the actor, as read.
struct EffectSeen
{
    std::uint32_t spell{0}; // the spell it belongs to; 0 for none
    float duration{0.0f};
    float elapsed{0.0f};
};

// The seconds left on the longest effect one of `sources` is running, or
// 0 for none. A power's source is itself; a shout's are its words' spells.
[[nodiscard]] float RemainingOn(std::span<const EffectSeen> effects, std::span<const std::uint32_t> sources) noexcept;

} // namespace ft
