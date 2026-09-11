#pragma once
// Small helpers for the game layer. Deliberately minimal.
//
// The temptation with an engine adapter is to build a utility layer before you
// know what actually repeats. These earn their place because every
// diagnostic line needs to name an actor, and every cooldown needs a clock.
// Anything else stays inline until it has repeated three times.

#include <chrono>
#include <string>

namespace RE
{
class Actor;
}

namespace ft::game
{

// Monotonic wall-clock seconds since the first call. Deliberately NOT game
// time: pacing must not stretch when the player sleeps or fast-travels.
[[nodiscard]] inline double NowSeconds()
{
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

// THE tactics clock: GAME time, expressed in real seconds at the current
// timescale. Every cooldown and every package lease is measured on this.
//
// Game time is the clock the game itself paces things by, and it already has
// every property we want: it stops in menus and while the world is frozen, so
// a request armed just before the panel opened is exactly as old when it
// closes; and it jumps on wait, sleep and fast travel, which expires every
// cooldown -- also what we want. Timescale (20 by default) is divided out at
// each step, so "2 s" means two real seconds of play whatever the timescale.
//
// Read from the hour-of-day global, not "hours passed": the latter is a float
// that loses sub-second precision after a few hundred game days, the former
// stays within 0..24 and precise. Day wraps are counted here. Any thread.
[[nodiscard]] double TacticsSeconds();

// "Lydia (000A2C94)" -- name plus FormID, because in a test cell you will have
// two Lydias (the real one and a placeatme copy) and the name alone is a lie.
[[nodiscard]] std::string Describe(RE::Actor *actor);

// Just the name, for the UI. Describe() above adds the FormID, which is what a
// log wants and what a screen does not.
[[nodiscard]] std::string DisplayNameOf(RE::Actor *actor);

} // namespace ft::game
