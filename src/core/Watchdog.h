#pragma once
// The pin watchdog's book-keeping across a fight, and its judgement of each
// pin and each ban on a tick, and the player's own book beneath the idle
// rules' pins. The game walks the followers, reads whether a
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

// One actor's books across a fight. On entering a fight the pins are
// remembered, twice: `before`, what comes back when the fight ends, and
// `opening`, what the fight's own rules pin over. The two differ by the
// Combat start rules' pins, which go into `opening` and last the fight; a
// request of the Normal layer during it -- the panel's word, the new
// normal -- goes into both. Any other combat rule's pin goes over
// `opening` in the book in force alone, and lasts while its rule holds
// (Lapse, against `opening`). On leaving, what the fight pinned is let go
// and what was there before comes back (Settle), and the book is what it
// was. What was there before is the player's own pins with the idle rules'
// over them: the idle rules are not asked in a fight, and their pins stand
// as the fight found them.
struct FightBook
{
    bool fighting{false};
    std::vector<Pin> before;
    std::vector<Pin> opening;

    // The tick's word on whether the actor is fighting. On the edge into
    // a fight the pins are remembered and nothing is returned; on the edge
    // out, the book becomes the remembered one and what changed is
    // returned; between edges, nothing.
    [[nodiscard]] std::optional<Settled> Note(std::vector<Pin> &pins, bool fightingNow);

    // A request mirrored into the books that hold its layer while a
    // fight is on: Normal into both, Opening into `opening`. Nothing
    // otherwise, and nothing for a standing combat rule's.
    void Mirror(Layer layer, PinRequest request, const Holdable &thing, Hand hands, bool moving, bool dualWield);
    // An Unequip, the same: what it reaches leaves the books that hold its
    // layer.
    void Release(Layer layer, Kind kind, Hand hands, BodyPart part);
    // A thing no longer carried, or no longer a form: its pin has nothing
    // to hold, here as in the book in force.
    void Forget(const Holdable &thing);

    // The remembered book while a fight is on, null otherwise: what a
    // rule's pin goes over, and what the panel shows as the player's own.
    [[nodiscard]] const std::vector<Pin> *Remembered() const noexcept
    {
        return fighting ? &before : nullptr;
    }
};

// One actor's own book, the Normal layer: what the panel pinned, and a
// Combat end rule. The idle rules pin over it, in the book in force and
// never in here, so when one of them stops holding (Lapse) what it
// displaced is still here to come back. And this, not the book in force,
// is what the save holds: a rule's pin is made again by its rule after a
// load, and never taken back as the player's.
struct IdleBook
{
    std::vector<Pin> normal;

    void Mirror(Layer layer, PinRequest request, const Holdable &thing, Hand hands, bool moving, bool dualWield);
    void Release(Layer layer, Kind kind, Hand hands, BodyPart part);
    void Forget(const Holdable &thing);
};

// One actor's pins in layers: the books beneath the one in force, and what
// each request does across all of them. The game keeps the book in force
// -- what the watchdog, the score hook and the equip detour read -- and one
// of these beside it, and asks here for every change to either (game/
// Pins.cpp), so which books a request is written into, and which book a
// lapse falls back to, are decided where they are tested. In a fight the
// order is fight over opening over idle over normal; out of one, idle over
// normal (Layer, core/Loadout.h).
struct Layers
{
    IdleBook idle;
    FightBook fight;

    // The book a layer's pin lies over, where what it displaces is still
    // held: the fight's opening under a standing combat rule, the book
    // from before the fight under a Combat start rule, the player's own
    // under an idle rule. None for the Normal layer, which lies over
    // nothing, and none for the fight's layers out of a fight.
    [[nodiscard]] const std::vector<Pin> *Beneath(Layer layer) const noexcept;

    // The tick's word on whether the actor is fighting (FightBook::Note).
    [[nodiscard]] std::optional<Settled> Note(std::vector<Pin> &pins, bool fightingNow)
    {
        return fight.Note(pins, fightingNow);
    }

    // A request of a layer: applied to the book in force, where it acts at
    // once whatever the layer, and to every book that holds the layer.
    // Returns what gave way in the book in force.
    std::vector<Displaced> Apply(std::vector<Pin> &pins, Layer layer, PinRequest request, const Holdable &thing,
                                 Hand hands, bool moving, bool dualWield);

    // An Unequip of a layer: every pin of the kind it reaches leaves the
    // book in force and every book that holds the layer. Returns those let
    // go in the book in force.
    std::vector<Pin> Release(std::vector<Pin> &pins, Layer layer, Kind kind, Hand hands, BodyPart part);

    // A list's turn: what its rules hold now (the evaluator's Standing),
    // settled against the book beneath its pins. None when the pins are
    // not the list's to let go: the idle list's in a fight, where they
    // stand under the fight's, and the combat list's out of one.
    [[nodiscard]] std::optional<Settled> Lapse(std::vector<Pin> &pins, bool combatList, const Wants &wants,
                                               bool dualWield);

    // A pin taken back from the save: the player's own, in force and
    // beneath.
    void Adopt(std::vector<Pin> &pins, const Holdable &thing, Hand hands);

    // What the save holds: the player's own, whatever the rules have over
    // it and whether or not a fight is on.
    [[nodiscard]] const std::vector<Pin> &Saved() const noexcept
    {
        return idle.normal;
    }

    // A pin with nothing left to hold, out of every book beneath: one left
    // there would come back with the next lapse and be dropped again.
    void Forget(const Holdable &thing);
    template <class Gone> void ForgetIf(Gone gone)
    {
        std::erase_if(idle.normal, gone);
        std::erase_if(fight.before, gone);
        std::erase_if(fight.opening, gone);
    }
};

// What the tick reads of one pin's thing.
struct PinSeen
{
    // Still theirs, as the engine answers for each kind: a copy of the
    // variant in the bag, for an item; known, for a spell, a power or a
    // shout.
    bool carried{true};
    bool on{false}; // worn where the pin says, readied in the voice, in the hand
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
    // A thing of the ban is in the bag: a row of the variant it names, or
    // any copy of the form where it names none; a spell, a power or a shout
    // known. The same question PinSeen asks, and
    // the same answer: a mark holds while there is something for it to
    // hold about.
    bool carried{true};
    bool on{false};     // worn, in a hand, or in the voice, anywhere
    bool pinned{false}; // a pin holds it: a rule's instruction wins while it lasts
};

enum class BanVerdict : std::uint8_t
{
    Keep,
    Drop,   // a ban on a variant with no row left: nothing to promise about
    TakeOff // found on, unpinned: the ban is kept by taking it off
};

// Over what was SEEN and nothing else: a ban on a variant and a ban on the
// form are judged the same, which is why the ban itself is not passed. The
// two differ in what `carried` means of them -- a row of the variant, or
// any copy of the form -- and that is the reader's question, not this one.
[[nodiscard]] BanVerdict JudgeBan(const BanSeen &seen) noexcept;

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
