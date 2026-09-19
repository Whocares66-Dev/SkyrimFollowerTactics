#pragma once
// Which of an actor's spells, shouts, powers and scrolls the snapshot
// calls known, which powers are used today, which spells are castable
// and so described and priced, and which spells and shouts are active by
// the effects running. The game reads the records and the effect list
// (game/Sensors.cpp, BuildSnapshot) and hands the facts here; the sorting
// is decided here, where it is tested. No Skyrim.

#include <cstdint>
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
        Scroll,
        Power,
        Spell
    };
    Kind kind{Kind::Spell};
    bool wrapper{false};   // a shout: one of ours, made for a power lease
    int highestWord{-1};   // a shout: the highest word unlocked, -1 for none
    int carried{1};        // a scroll: how many
    bool castable{true};   // a spell: one the AI can cast (IsCastable)
    bool greater{false};   // a power: a greater power, once a day
    bool usedToday{false}; // a greater power: on the engine's used list
};

struct SpellsKnown
{
    std::vector<std::uint32_t> known;     // in the order seen
    std::vector<std::uint32_t> usedToday; // greater powers used today
    std::vector<std::uint32_t> castable;  // the spells for the loadout and the pricing
};

// A shout is known with a word unlocked and not a wrapper: a rule that
// names one with no word reports it missing rather than firing into
// silence. A scroll is known while carried: knowing and carrying are the
// one question for it. A power is known, and a greater one used today is
// said so. A spell is known when castable, and is then the loadout's and
// priced.
[[nodiscard]] SpellsKnown ClassifySpells(std::span<const SpellSeen> seen);

// One effect running on the actor, as read.
struct EffectSeen
{
    std::uint32_t spell{0}; // the spell it belongs to; 0 for none
    float duration{0.0f};
    float elapsed{0.0f};
};

// A shout's words' spells, for a running effect to be the shout's.
struct ShoutWords
{
    std::uint32_t shout{0};
    std::vector<std::uint32_t> words;
};

// The spells active by the effects running: an effect with a spell, a
// duration and time left, each its spell's id; and where the spell is a
// shout's word, the shout's too, so a Become Ethereal rule waits while it
// holds. Instant effects have already happened and never lapse, so
// treating them as still up would block a rule forever.
[[nodiscard]] std::vector<std::uint32_t> ActiveSpells(std::span<const EffectSeen> effects,
                                                      std::span<const ShoutWords> shouts);

// The seconds left on the longest effect one of `sources` is running, or
// 0 for none. A power's source is itself; a shout's are its words' spells.
[[nodiscard]] float RemainingOn(std::span<const EffectSeen> effects, std::span<const std::uint32_t> sources) noexcept;

} // namespace ft
