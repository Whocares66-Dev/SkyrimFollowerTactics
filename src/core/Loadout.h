#pragma once
// Pins, and what follows from them. Pure: no Skyrim, no SKSE, no CommonLib;
// covered by tests/test_loadout.cpp.
//
// A pin says "this stays in this hand" (or "this stays on", for a thing with
// no hand). Everything else is derived from the pins and from a plain
// description of what she has: which hands a thing could take, whether the
// combat AI would choose it at all, which body slots a piece of armour
// covers. The game layer builds that description from her records, asks
// these functions, and imposes the answers on the engine. Nothing here knows
// how; that is what keeps it testable.
//
// A pin keeps its promise by three means, each covering what the others
// cannot. An ITEM is equipped with the engine's prevent-removal flag, which
// stops the engine's own equip-best swap (verified: pinned robes kept iron
// armour off). The combat AI's own list of options is PRUNED of everything
// that competes for a pinned hand, since that list, not her records, is what
// the AI chooses from in a fight (verified: a removed spell was cast from it,
// a pruned dagger never was). And a WATCHDOG puts back, out of combat,
// whatever got past both. A spell has no flag, so for it the last two carry
// the promise alone.

#include <cstdint>
#include <vector>

namespace ft
{

// Which hand, or hands: a bit set, Both being Left and Right together.
enum class Hand : std::uint8_t
{
    None = 0,
    Left = 1,
    Right = 2,
    Both = 3
};

[[nodiscard]] constexpr Hand operator|(Hand a, Hand b) noexcept
{
    return static_cast<Hand>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

[[nodiscard]] constexpr bool Overlap(Hand a, Hand b) noexcept
{
    return (static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b)) != 0;
}

// Which hands a thing's record lets it take.
enum class Grip : std::uint8_t
{
    None,      // armour, ammunition, a potion, a power: no hand
    LeftOnly,  // a shield, a torch, an NPC's left-hand spell variant
    RightOnly, // an NPC's right-hand spell variant
    Either,    // a one-handed weapon; most spells
    Both       // a two-hander, a bow, a crossbow, a master spell
};

// One thing she has, as the planner sees it.
struct Holdable
{
    std::uint32_t form{0};
    Grip grip{Grip::None};
    // A spell above her skill. The combat AI will not choose it on its own,
    // so a pin on it would be a promise unkept: it can be equipped, not
    // pinned.
    bool unusable{false};
    // The body slots a piece of armour covers, for armour against armour;
    // 0 for everything else.
    std::uint32_t slots{0};
    bool ammo{false};
};

struct Pin
{
    std::uint32_t form{0};
    Hand hands{Hand::None};
};

// The hands a thing takes when pinned, given the hand asked for. A thing
// that takes one particular hand, or both, takes that whatever was asked;
// a thing that takes either goes to the hand asked for, the right when
// none was.
[[nodiscard]] Hand HandsFor(Grip grip, Hand requested) noexcept;

// Can it be pinned at all?
[[nodiscard]] bool Pinnable(const Holdable &thing) noexcept;

// Does pinning `incoming` to `hands` mean releasing `held`, pinned to
// `heldHands`? Hands that overlap; armour on shared body slots; ammunition
// against ammunition. Anything else lives alongside.
[[nodiscard]] bool Conflicts(const Holdable &incoming, Hand hands, const Holdable &held, Hand heldHands) noexcept;

// Would a thing with this grip take a hand the pins hold? Such a thing must
// not be offered to the combat AI. A one-hand-only thing competes with a
// pin on its hand. A thing that takes either hand, or both, competes with
// a pin on ANY hand: the AI puts an either-hand spell into a pinned hand
// as readily as the free one. So with both hands pinned the AI is left
// with the pins and nothing else, which is the intent.
[[nodiscard]] bool Competes(Grip grip, Hand pinned) noexcept;

// The hands the pins hold, all together.
[[nodiscard]] Hand PinnedHands(const std::vector<Pin> &pins) noexcept;

// What the combat AI must not be offered: every unpinned thing that
// competes for a pinned hand.
[[nodiscard]] std::vector<std::uint32_t> KeepFromAI(const std::vector<Pin> &pins, const std::vector<Holdable> &things);

} // namespace ft
