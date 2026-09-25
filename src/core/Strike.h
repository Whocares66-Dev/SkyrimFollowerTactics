#pragma once
// A follower's power attack, from the request to the swing's end: wait for
// the weapon drawn, no swing of their own, and the target inside the
// attack's strike angle; take the power attack as the combat AI takes one,
// the right attack action carrying the attack's event (dev/ATTACK.md); then
// follow it to its hit and its end. The game side reads the actor as their
// animation graph's events come, and on the half-second turn, and performs
// the action (game/Blows.cpp); the steps, the waits and the reason it is
// over are decided here, where every one is tested. No Skyrim.
//
// The action answers at once -- the engine takes it or turns it away -- so
// the step performs it through a callback and reads the answer in the same
// step. The test's callback is a script of answers.

#include "GraphEvents.h"

#include <cstdint>
#include <functional>
#include <string>

namespace ft
{

// From the request to the action taken: long enough for a swing of their
// own to end and their combat controller to bring the target round; a
// request still waiting then is given up, and says at what.
inline constexpr double kStrikeDeadlineSeconds = 2.0;
// From the action taken to the swing's end: a power attack's animation is
// under two seconds, the dual-wield one's three swings included.
inline constexpr double kStrikeWatchSeconds = 3.0;

enum class StrikeStep : std::uint8_t
{
    Ready,   // waiting for the weapon drawn, no swing and the target in front, then the action
    Striking // the action was taken; followed to its hit and its end
};

struct StrikeState
{
    StrikeStep step{StrikeStep::Ready};
    double requestedAt{0.0};
    double sentAt{-1.0};
    double hitAt{-1.0};
    double endAt{-1.0};
    bool waited{false};    // a step could not take the action at once
    bool sawAttack{false}; // an attack state seen once the action was taken
    int refusals{0};       // the action turned away
    // The graph's counts when the action was taken: one since is this
    // attack's.
    int hitFramesBefore{0};
    int powerStopsBefore{0};
    int attackStopsBefore{0};
};

[[nodiscard]] StrikeState RequestStrikeAt(double now) noexcept;

// What the step reads of the follower.
struct StrikeSeen
{
    bool holder{true}; // the follower still resolves
    bool weaponDrawn{false};
    bool attacking{false}; // an attack state other than none
    bool casting{false};   // a shout or a spell of their own in progress
    bool facing{false};    // the target inside the attack's strike angle of their heading
    // Their graph's events, counted: an attack's hit frame, a power
    // attack's own end, any attack's end.
    int hitFrames{0};
    int powerStops{0};
    int attackStops{0};
};

// The counts, from the request's watch on the follower's graph.
void Hear(StrikeSeen &seen, const Heard &heard) noexcept;

// A step and what it read, as its log line says it: "step at 123.456:
// holder=1 drawn=1 attacking=0 casting=0 facing=1 hitFrames=0
// powerStops=0 attackStops=0".
[[nodiscard]] std::string ReadsOf(const StrikeSeen &seen, double now);

// One step, where the request can take it; the reason it is over, or null
// while it goes on. `perform` takes the action and answers whether the
// engine took it.
[[nodiscard]] const char *AdvanceStrike(StrikeState &state, const StrikeSeen &seen, double now,
                                        const std::function<bool()> &perform);

} // namespace ft
