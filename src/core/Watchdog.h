#pragma once
// The pin watchdog's book-keeping across a fight, and its judgement of each
// pin and each ban on a tick. The game walks the followers, reads whether a
// thing is on and whether a copy is still carried, and performs what is
// decided (game/Pins.cpp, EnforcePins); what to remember when a fight
// begins, what to give back when it ends, what a panel edit during it
// changes, and whether a pin or a ban asks for anything this tick are
// decided here, where they are tested. No Skyrim.

#include "Loadout.h"

#include <optional>
#include <vector>

namespace ft
{

// One actor's book across a fight. On entering a fight the pins are
// remembered; a rule's pin during the fight goes over them for the fight
// only, and the panel's word during it is the new normal, so a panel
// request goes into the remembered book as well; on leaving, what the
// fight pinned is let go and what was there before comes back
// (SettleAfterFight), and the book is what it was.
struct FightBook
{
    bool fighting{false};
    std::vector<Pin> before;

    // The tick's word on whether the actor is fighting. On the edge into
    // a fight the pins are remembered and nothing is returned; on the edge
    // out, the book becomes the remembered one and what changed is
    // returned; between edges, nothing.
    [[nodiscard]] std::optional<AfterFight> Note(std::vector<Pin> &pins, bool fightingNow);

    // The panel's request, mirrored into the remembered book while a fight
    // is on; nothing otherwise.
    void Mirror(PinRequest request, const Holdable &thing, Hand hands, bool moving, bool dualWield);

    // The remembered book while a fight is on, null otherwise: what a
    // rule's pin goes over, and what the panel shows as the player's own.
    [[nodiscard]] const std::vector<Pin> *Remembered() const noexcept
    {
        return fighting ? &before : nullptr;
    }
};

// What the tick reads of one pin's thing.
struct PinSeen
{
    bool carried{true}; // a copy of the variant is in the bag (an item; a spell or a voice is always carried)
    bool on{false};     // worn where the pin says, readied in the voice, in the hand
};

enum class PinVerdict : std::uint8_t
{
    Keep,
    Drop,   // no copy left: the pin has nothing to hold
    PutBack // taken off by the game: put it back now (PutBackNow)
};

[[nodiscard]] PinVerdict JudgePin(const Pin &pin, const PinSeen &seen, bool fighting, bool castInProgress) noexcept;

// What the tick reads of one ban's thing.
struct BanSeen
{
    bool carried{true}; // a row of the banned variant is in the bag (only a ban on a variant asks)
    bool on{false};     // worn, in a hand, or in the voice, anywhere
    bool pinned{false}; // a pin holds it: a rule's instruction wins while it lasts
};

enum class BanVerdict : std::uint8_t
{
    Keep,
    Drop,   // a ban on a variant with no row left: nothing to promise about
    TakeOff // found on, unpinned: the ban is kept by taking it off
};

[[nodiscard]] BanVerdict JudgeBan(const Banned &ban, const BanSeen &seen) noexcept;

} // namespace ft
