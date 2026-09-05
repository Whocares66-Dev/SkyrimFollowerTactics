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
// cannot, and all of them ours: nothing is written onto the item, so a
// save played without the mod carries no pin. The engine's own EQUIP is
// detoured, and one of its choosing -- the best weapon on leaving combat,
// the outfit on a cell change -- is refused when it would take a pinned
// hand or slot. The combat AI SCORES every option in its list each time it
// decides what to hold, and that score is answered by us: zero for anything
// that competes for a pinned hand, so it is never chosen (verified: a pinned
// bow held at melee range against a sword the AI kept re-listing). And a
// WATCHDOG puts back, out of combat, whatever got past both. (The engine's
// prevent-removal flag once doubled the first for items; it lives on the
// worn item in the save and outlived the mod, so it is no longer set.)

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

// The hands left once one is let go.
[[nodiscard]] constexpr Hand Without(Hand hands, Hand hand) noexcept
{
    return static_cast<Hand>(static_cast<std::uint8_t>(hands) & ~static_cast<std::uint8_t>(hand));
}

// The hands two sets share.
[[nodiscard]] constexpr Hand Common(Hand a, Hand b) noexcept
{
    return static_cast<Hand>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
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

// The hands a thing with this grip could go into.
[[nodiscard]] constexpr Hand Reach(Grip grip) noexcept
{
    switch (grip)
    {
    case Grip::LeftOnly:
        return Hand::Left;
    case Grip::RightOnly:
        return Hand::Right;
    case Grip::Either:
    case Grip::Both:
        return Hand::Both;
    case Grip::None:
    default:
        return Hand::None;
    }
}

// What a thing is to a rule: the four kinds an equip rule names. A shield
// or a torch is a Weapon here, as the panel lists it, because it is chosen
// with the sword and takes a hand; Armor is what is worn.
enum class Kind : std::uint8_t
{
    Other,
    Weapon,
    Spell,
    Armor,
    Ammo
};

// One thing she has, as the planner sees it.
struct Holdable
{
    std::uint32_t form{0};
    Kind kind{Kind::Other};
    Grip grip{Grip::None};
    // A spell above her skill. The combat AI will not choose it on its own,
    // so a pin on it would be a promise unkept: it can be equipped, not
    // pinned.
    bool unusable{false};
    // The body slots a piece of armour covers, for armour against armour;
    // 0 for everything else.
    std::uint32_t slots{0};

    [[nodiscard]] constexpr bool IsAmmo() const noexcept
    {
        return kind == Kind::Ammo;
    }
};

[[nodiscard]] const Holdable *FindHoldable(const std::vector<Holdable> &things, std::uint32_t form) noexcept;

// Does a pin in `pinned` hold every hand in `wanted`? None is held by
// anything: a thing with no hand asks for no hand.
[[nodiscard]] constexpr bool Covers(Hand pinned, Hand wanted) noexcept
{
    return Common(pinned, wanted) == wanted;
}

// A thing pinned, and the hands it is pinned in: None for armour and
// ammunition, which have no hand; Both for a two-hander, or for an
// either-hand thing pinned once in each hand.
struct Pin
{
    Holdable thing;
    Hand hands{Hand::None};
};

[[nodiscard]] Pin *FindPin(std::vector<Pin> &pins, std::uint32_t form) noexcept;
[[nodiscard]] const Pin *FindPin(const std::vector<Pin> &pins, std::uint32_t form) noexcept;

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

// Would a thing with this grip take a hand the pins hold, when the hand it
// would go into is not known? A one-hand-only thing competes with a pin on
// its hand. A thing that takes either hand, or both, competes with a pin
// on ANY hand: the AI puts an either-hand spell into a pinned hand as
// readily as the free one. The coarse rule; KeptFromAI is the fine one.
[[nodiscard]] bool Competes(Grip grip, Hand pinned) noexcept;

// The hands the pins hold, all together.
[[nodiscard]] Hand PinnedHands(const std::vector<Pin> &pins) noexcept;

// ---- What a request from the panel does to the pins. `hands` is what
// HandsFor gave the request: the hand clicked as the thing takes it, None
// for a thing with no hand. The game layer does the equipping; these keep
// the book.

// What other pins give up so that `thing` can be pinned in `hands`, each
// with the hands let go (None for a no-hand pin, which goes whole). A pin
// of one-handers holding both hands -- two daggers -- gives up only the
// hand asked for; a two-hander, or a pin with no hand, goes whole.
struct Displaced
{
    std::uint32_t form{0};
    Hand hands{Hand::None};
};
[[nodiscard]] std::vector<Displaced> MakeRoom(std::vector<Pin> &pins, const Holdable &thing, Hand hands);

// Pin `thing` in `hands`. An either-hand thing already pinned in the other
// hand is pinned in both, a spell once in each -- unless `moving`, her one
// weapon changing hands, which leaves the first hand.
void AddPin(std::vector<Pin> &pins, const Holdable &thing, Hand hands, bool moving);

// A release or a take-off of `thing` in `hands`. A hand cell acts on THAT
// hand and no other: the pin lets it go if it holds it and keeps the rest;
// a pin on the other hand alone is not touched. With no hand named the
// whole pin goes. Returns the hands the request acts on.
[[nodiscard]] Hand LetGo(std::vector<Pin> &pins, const Holdable &thing, Hand hands);

// Must this entry of the combat AI's list be taken from it? The AI's list
// holds a thing once per hand it could go into, an either-hand spell as a
// left entry and a right entry, and each entry carries its hand. An entry
// whose hand a pin holds goes, unless it is that pin itself: Flames pinned
// left keeps Flames-in-left and loses Flames-in-right. An entry that
// carries no hand falls back to the coarse rule.
[[nodiscard]] bool KeptFromAI(const std::vector<Pin> &pins, const Holdable &thing, Hand slot) noexcept;

// Is a thing entirely unavailable to the AI: every hand it could take is
// spoken for? What the panel greys out. Armour is not on the AI's list at
// all, but a pinned piece holds its body slots against the engine's own
// swap, so armour over the same slot is set aside the same way, and
// ammunition against pinned ammunition.
[[nodiscard]] bool SetAside(const std::vector<Pin> &pins, const Holdable &thing) noexcept;

// Why: the pins holding a hand the thing could take, each cut down to the
// hands it takes from this thing. The panel's answer to "why is this row
// greyed out". Empty for the thing's own pin, and for anything not set
// aside.
[[nodiscard]] std::vector<Pin> Shadowing(const std::vector<Pin> &pins, const Holdable &thing);

// ---- The watchdog's decision, each tick, for one pin.
//
// A pin the game has taken off goes back on -- except a HAND pin while one
// of our own casts is in progress: the UseMagic package puts its spell in a
// hand, and a pinned dagger comes off the moment a spell wants the hand
// (the equip detour answers the engine's equip-best swap, not a spell
// equip). The cast is a borrow: once it has run, the
// dagger goes back. Until it has, putting it back would knock the spell
// out of the hand mid-cast. Armour and ammunition are contested by no cast
// and hold throughout. Out of a fight, everything goes straight back; the
// combat AI's own spell choice is kept off a pinned hand by the score hook,
// so a fight no longer stands the hand pins down for its whole length --
// that left a mage dagger-less from the first cast to the end of the fight
// (2026-09-04, Marcurio's daggers and Lightning Bolt).
[[nodiscard]] bool PutBackNow(const Pin &pin, bool on, bool fighting, bool castInProgress) noexcept;

// ---- The fight is over. What the book held when it began comes back, and
// what the fight pinned is let go.

// A fight's pin let go: left on and unpinned, the AI's to change; or taken
// off, because a pin from before the fight is coming back to that hand or
// slot.
struct Released
{
    std::uint32_t form{0};
    Hand hands{Hand::None};
    bool takeOff{false};
};

struct AfterFight
{
    std::vector<Released> released; // the fight's pins, in `now` and not in `before`
    std::vector<Pin> restored;      // the pins from before that the fight displaced
};

// With `before` empty nothing is taken off: the fight's gear stays on,
// unpinned. With pins in `before`, only those come back, and a fight's
// pin comes off only to make way for one of them. A pin in both -- kept
// through the fight, or made in the panel during it -- is untouched.
[[nodiscard]] AfterFight SettleAfterFight(const std::vector<Pin> &now, const std::vector<Pin> &before);

} // namespace ft
