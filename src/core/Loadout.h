#pragma once
// Pins, and what follows from them. Pure: no Skyrim, no SKSE, no CommonLib;
// covered by tests/test_loadout.cpp.
//
// A pin says "this stays in this hand" (or "this stays on", for a thing with
// no hand). Everything else is derived from the pins and from a plain
// description of what they have: which hands a thing could take, whether the
// combat AI would choose it at all, which body slots a piece of armour
// covers. The game layer builds that description from their records, asks
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
// WATCHDOG puts back, in a fight or out of one, whatever got past both --
// a hand pin waits only while one of our casts holds the hand. (The engine's
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

// What a thing is to a rule: the four kinds an equip rule names, and Voice.
// A shield or a torch is a Weapon here, as the panel lists it, because it is
// chosen with the sword and takes a hand; Armor is what is worn. Voice is a
// power or a shout: readied in the one voice slot, no hand, so one voice pin
// displaces another as one quiver displaces another. The panel pins these;
// no rule names them (an Equip power action would promise what the vanilla
// AI never reaches for).
enum class Kind : std::uint8_t
{
    Other,
    Weapon,
    Spell,
    Armor,
    Ammo,
    Voice
};

// One thing they have, as the planner sees it.
struct Holdable
{
    std::uint32_t form{0};
    Kind kind{Kind::Other};
    Grip grip{Grip::None};
    // A spell above their skill. The combat AI will not choose it on its own,
    // so a pin on it would be a promise unkept: it can be equipped, not
    // pinned.
    bool unusable{false};
    // The body slots a piece of armour covers, for armour against armour;
    // 0 for everything else.
    std::uint32_t slots{0};
    // How many they carry: an item's count, and "plenty" for a spell,
    // which can be in both hands at once. A single item pinned in one hand
    // has no second copy for the other, and the engine, asked for one,
    // shows the same object in both hands (the doubled dagger).
    int count{1};

    [[nodiscard]] constexpr bool IsAmmo() const noexcept
    {
        return kind == Kind::Ammo;
    }
    [[nodiscard]] constexpr bool IsVoice() const noexcept
    {
        return kind == Kind::Voice;
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

// The forms the follower must never use (the bans, below).
using Bans = std::vector<std::uint32_t>;

[[nodiscard]] Pin *FindPin(std::vector<Pin> &pins, std::uint32_t form) noexcept;
[[nodiscard]] const Pin *FindPin(const std::vector<Pin> &pins, std::uint32_t form) noexcept;

// The hands a thing takes when pinned, given the hand asked for. A thing
// that takes one particular hand, or both, takes that whatever was asked;
// a thing that takes either goes to the hand asked for, the right when
// none was.
[[nodiscard]] Hand HandsFor(Grip grip, Hand requested) noexcept;

// Can it be pinned at all?
[[nodiscard]] bool Pinnable(const Holdable &thing) noexcept;

// Dual wielding is a one-handed weapon in each hand, and a combat style may
// forbid it: the AI then never holds two, and a pin that made them would
// be a stance the style cannot fight in. Would putting `thing` into a hand
// make one, given what the other hand holds (null for nothing)? A shield,
// a torch, a spell, a two-hander there is no bar, nor is an armour or a
// spell coming in. The only copy of the very weapon the other hand holds
// is a MOVE across, not a second weapon. The caller asks only where the
// style forbids it.
//
// What is EQUIPPED in the other hand greys nothing: the AI swaps weapons
// as it likes, and a second one-hander offered beside an equipped one is
// no different from one offered for the same hand. A PINNED one-hander
// greys the other one-handers. Either way, one pinned or equipped into the
// other hand takes the first off -- its pin, through Conflicts and
// MakeRoom with `dualWield` false, and the weapon itself on the game side
// -- so the follower never stands with a weapon in each.
[[nodiscard]] bool WouldDualWield(const Holdable &thing, const Holdable *inOtherHand) noexcept;

// Does pinning `incoming` to `hands` mean releasing `held`, pinned to
// `heldHands`? Hands that overlap; armour on shared body slots; ammunition
// against ammunition; a voice pin against a voice pin. Anything else lives
// alongside.
[[nodiscard]] bool Conflicts(const Holdable &incoming, Hand hands, const Holdable &held, Hand heldHands,
                             bool dualWield = true) noexcept;

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
[[nodiscard]] std::vector<Displaced> MakeRoom(std::vector<Pin> &pins, const Holdable &thing, Hand hands,
                                              bool dualWield = true);

// Pin `thing` in `hands`. An either-hand thing already pinned in the other
// hand is pinned in both, a spell once in each -- unless `moving`, their one
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
// left keeps Flames-in-left and loses Flames-in-right. A pinned thing's
// entry for a hand its pin does not hold goes too when there is only one of
// it: the staff pinned right has no second staff for the left. An entry
// that carries no hand falls back to the coarse rule.
[[nodiscard]] bool KeptFromAI(const std::vector<Pin> &pins, const Holdable &thing, Hand slot) noexcept;

// Would the engine's own equip of `thing` into `into` -- the slot's hand,
// None for a thing with none -- break a ban or a pin? What the equip
// detours ask (game/Pins.cpp) of an item, a spell and a shout alike, and
// what the test's engine does. A banned thing is refused outright. The
// pinned thing itself passes into its own hand, and into another as the
// AI is kept from it (KeptFromAI): with one copy the engine would show it
// in both hands, and a pin may hold that hand. Anything else is refused
// where it conflicts with a pin; a thing with no hand and no slot never
// does. The answer says why, for the log, and which pin where one is in
// the way.
struct Refusal
{
    enum class Why : std::uint8_t
    {
        None,
        Banned,   // the thing is banned, whatever the hands
        OneCopy,  // its own pin, and no second copy for the other hand
        OtherPin, // its own pin, and another pin holds the hand asked
        Conflict  // another pin's hand or slot
    };
    Why why{Why::None};
    const Pin *pin{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return why != Why::None;
    }
};
[[nodiscard]] Refusal RefusesEngineEquip(const std::vector<Pin> &pins, const Bans &bans, const Holdable &thing,
                                         Hand into, bool dualWield) noexcept;

// What a request from the panel does to a book of pins, the same on the
// book in use and on the one remembered for after the fight. Pin makes
// room and adds; Equip makes room, and a lone weapon moving hands
// (`moving`) leaves the hand it came from; Ban lets the whole pin go.
// Returns what gave way to make room, for the log.
enum class PinRequest : std::uint8_t
{
    Pin,
    Equip,
    Ban
};
[[nodiscard]] std::vector<Displaced> ApplyRequest(std::vector<Pin> &pins, PinRequest request, const Holdable &thing,
                                                  Hand hands, bool moving, bool dualWield);

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

// ---- Bans: what the AI must never use.
//
// A ban is the pin's opposite: not "this stays on" but "this never goes on".
// The same three means keep it -- the score hook answers zero for a banned
// entry, the equip detour refuses the engine's equip of a banned thing, and
// the watchdog takes off a banned thing found on -- and, as with a pin,
// nothing is written to the thing or the follower, so a save played without
// the mod carries no ban. A ban and a pin on one thing cannot both hold:
// the panel's ban lets the pin go. A rule's pin on a banned thing is the
// player's own instruction and wins for as long as it lasts; the watchdog
// leaves a pinned thing alone whatever the bans say.

[[nodiscard]] bool IsBanned(const Bans &bans, std::uint32_t form) noexcept;
// Each returns whether the book changed.
bool Ban(Bans &bans, std::uint32_t form);
bool Unban(Bans &bans, std::uint32_t form);

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
