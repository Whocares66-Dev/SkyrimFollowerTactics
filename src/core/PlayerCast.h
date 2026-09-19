#pragma once
// The player's cast, from the tick's side (dev/PLAYER.md): the spell lent
// to a hand or the power to the voice, the hands drawn, the control pressed
// once the player's own doing lets it, the charge watched to Ready, the
// release, the fire event, and the hands given back once the caster is
// idle. The game side reads the player each fast tick and sends the
// presses and equips (game/PlayerCast.cpp); the steps, their windows and
// the reason the run is over are decided here, where they are tested. No
// Skyrim.
//
// The commands have no answer the step needs -- a press is sent, an equip
// is asked for, and the next tick's reading says what came of it -- so
// they go out through a callback that performs them, which the test
// records.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ft
{

// Each step's window, from the moment the step began. Long enough for the
// thing asked for to show, short enough that the hand is not held for long
// by a cast that is not coming: a lent hand shows the spell on the next
// update; a draw is under a second; a charge is the spell's own time (a
// master spell's is seconds) and a little; a fire-and-forget spell leaves
// the hand within a second of the release.
inline constexpr double kLendSeconds = 1.0;
inline constexpr double kDrawSeconds = 2.0;
inline constexpr double kHandsFreeSeconds = 2.0;
inline constexpr double kChargeSlackSeconds = 2.0;
inline constexpr double kFireSeconds = 2.0;
inline constexpr double kSettleSeconds = 2.0;
// A hand's cast released and its caster idle again this soon after is one
// that ended without firing.
inline constexpr double kFireGraceSeconds = 0.1;
// How long the shout control is held for the words, at most: the engine
// charges a word about every half second of a hold, and fires whatever is
// charged on the release.
inline constexpr double kWordsSeconds = 3.0;

enum class CastStep : std::uint8_t
{
    Lending,   // the spell put in the hand, or the power in the voice; waiting for it to show
    Drawing,   // the hands drawn if they were sheathed; waiting for drawn
    Pressing,  // waiting for the hands to be free of the player's own doing, then the press
    Charging,  // pressed; watching the caster for Ready (or, for a stream, for Casting)
    Holding,   // a stream: held for its sustain, then released
    Firing,    // released; waiting for the fire event
    Restoring, // waiting for the caster to be idle, then the hands given back
};
[[nodiscard]] const char *ToString(CastStep step) noexcept;

// The run's standing. What it casts and how -- voice or hand, a shout or a
// power, dual, a stream -- is the request's and set before the first step.
struct CastState
{
    bool voice{false}; // by the shout control, not a hand's
    bool shout{false}; // the voice form is a shout (else a power)
    bool dual{false};  // from both hands at once
    bool sustained{false};
    float sustain{0.0f};
    float chargeTime{0.0f}; // the spell's own, added to the charge window
    // For a shout: the highest word unlocked (0 for the first alone),
    // which the control is held for; and the level it went off at.
    int wordsWanted{0};
    int wordsHeld{-1};
    // For a power: on the engine's used-power list before the press. A
    // power goes off on the release with no voice animation and no fire
    // event, and the list taking it is what says it fired.
    bool usedBefore{false};
    bool lendAsked{false};
    bool drew{false};
    bool pressed{false};
    bool released{false};
    bool fired{false};
    CastStep step{CastStep::Lending};
    double requestedAt{0.0};
    double stepAt{0.0};
    double pressedAt{-1.0};
    double readyAt{-1.0};
    double releasedAt{-1.0};
    double firedAt{-1.0};
    // The highest caster state seen after the press: what the charge got
    // to, for a cast that never fired.
    int highestState{-1};
    // Why the run ended, set by the step that ended it; the outcome is
    // `fired`.
    std::string reason;
};

// What the tick reads of the player. The game reads only what the step
// needs: the refusal is asked of the engine on the Pressing step alone.
struct CastSeen
{
    bool player{true};  // resolves, with an actor state
    bool placed{false}; // the spell in every hand the run takes, or the form in the voice
    enum class Weapon : std::uint8_t
    {
        Drawn,
        Sheathed,
        Other // drawing or sheathing
    };
    Weapon weapon{Weapon::Other};
    // The caster asked about: idle (no spell, state none); its state; and
    // whether it holds a spell. Others idle: the other hand's and the
    // voice's, which a dual press wants.
    bool casterIdle{false};
    enum class Caster : std::uint8_t
    {
        None,
        Casting,
        Ready,
        Other
    };
    Caster caster{Caster::None};
    int casterState{0}; // the engine's number, for the report
    bool casterHasSpell{false};
    bool othersIdle{true};
    bool attacking{false};
    bool blocking{false};
    bool buttonHeld{false};       // the player's own attack button down
    const char *refusal{nullptr}; // why the engine refuses the cast now, or null
    int wordsCharged{-1};         // a shout's, else -1
    bool onUsedList{false};       // a power on the used list now
    bool fireSeen{false};         // our spell's fire event, or the voice's
};

enum class CastCommand : std::uint8_t
{
    LendVoice,    // the shout or power into the voice slot
    LendHands,    // the spell into the hand or hands
    Draw,         // the hands out
    Press,        // the press: the shout control, or the hand's, or both
    HoldPress,    // the shout control held on, with the time since the press
    ReplayPress,  // a single hand's press held on, for the handler's pairing window
    Release,      // the release
    MarkPowerUsed // the power onto the used list
};

// ---- Giving the hands and the voice back.
//
// What a hand held before the cast borrowed it, as the game read it.
struct HeldSlot
{
    bool lent{false};       // this hand was borrowed; the others are not touched
    std::uint32_t spell{0}; // the spell it held; 0 for none
    std::uint32_t item{0};  // the item it held; 0 for none
    bool twoHanded{false};  // the item fills both hands, so it goes back once
};

enum class RestoreWhat : std::uint8_t
{
    Spell,       // put the spell back in this hand
    Item,        // put the item back in this hand
    KeepBorrowed // the hand was empty: it keeps what it was lent
};

struct HandRestore
{
    bool left{false};
    RestoreWhat what{RestoreWhat::KeepBorrowed};
    std::uint32_t form{0};
};

// What each borrowed hand gets back, the left hand first. A hand that held
// nothing keeps the spell it was lent: unequipped after the cast, the next
// lend into that hand played the equip animation twice (2026-09-18), and a
// hand that was empty was holding nothing the player chose. A two-handed
// weapon reads from both hands and is put back once; two copies of ONE
// one-handed form are two weapons and are both put back (before
// 2026-09-19 the second was dropped, since only the form was compared).
[[nodiscard]] std::vector<HandRestore> PlanRestore(const HeldSlot &left, const HeldSlot &right);

// What the voice gets back: what it held before, or, where it held
// nothing, the shout or power we lent it is taken off -- unlike a hand,
// the voice slot does not keep it.
enum class VoiceRestore : std::uint8_t
{
    None,         // the voice was not borrowed
    PutBack,      // what it held before
    ReleaseShout, // it held nothing and we lent a shout
    ReleasePower  // it held nothing and we lent a power
};
[[nodiscard]] VoiceRestore PlanVoiceRestore(bool lent, bool hadBefore, bool weLentAShout) noexcept;

// One step, where the run can take it; the reason it is over, or null
// while it goes on. Each step's window is measured from when the step
// began.
[[nodiscard]] const char *AdvancePlayerCast(CastState &state, const CastSeen &seen, double now,
                                            const std::function<void(CastCommand)> &perform);

} // namespace ft
