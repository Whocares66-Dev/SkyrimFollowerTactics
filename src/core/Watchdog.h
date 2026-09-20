#pragma once
// The pin watchdog's book-keeping across a fight, and its judgement of each
// pin and each ban on a tick. The game walks the followers, reads whether a
// thing is on and whether a copy is still carried, and performs what is
// decided (game/Pins.cpp, EnforcePins); what to remember when a fight
// begins, what to give back when it ends, what a panel edit during it
// changes, and whether a pin or a ban asks for anything this tick are
// decided here, where they are tested. No Skyrim.

#include "Loadout.h"

#include <cstddef>
#include <optional>
#include <span>
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

// ---- One actor's pass, in the order it is performed.
//
// The pins first, then the bans: the tick a fight ends, the fight's own
// pins have just been let go, and a banned thing a rule had pinned for
// the fight is found here unpinned and taken off in the same pass. A cast
// of ours holding a hand suspends the bans altogether, as it suspends a
// hand pin: taking the thing off would cut the cast.
enum class WatchAct : std::uint8_t
{
    PutBack, // a pin's thing, off: put it back
    TakeOff  // a banned thing, on and unpinned: take it off
};

struct WatchStep
{
    WatchAct act{WatchAct::PutBack};
    std::size_t index{0}; // into the pins for PutBack, into the bans for TakeOff
    // A ban enforced on a thing whose rule pin has just gone with the
    // fight: the fight ending, not a promise broken.
    bool afterFight{false};
};

struct WatchPlan
{
    std::vector<std::size_t> dropPins; // no copy of the pinned thing left
    std::vector<std::size_t> dropBans; // a ban on a variant with no row left
    std::vector<WatchStep> steps;
};

// `pinsSeen` and `bansSeen` are what the game read of each pin's and each
// ban's thing, in the books' order. `lapsed` are the forms the end of a
// fight has just let go.
[[nodiscard]] WatchPlan PlanWatch(const std::vector<Pin> &pins, std::span<const PinSeen> pinsSeen, const Bans &bans,
                                  std::span<const BanSeen> bansSeen, bool fighting, bool castInProgress,
                                  std::span<const std::uint32_t> lapsed);

} // namespace ft
