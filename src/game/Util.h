#pragma once
// Small helpers for the game layer. Deliberately minimal.
//
// The temptation with an engine adapter is to build a utility layer before you
// know what actually repeats. These two earn their place because every
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

// Monotonic seconds since the first call. Deliberately NOT game time: cooldowns
// are about pacing the mod's own actions, so they must not stretch when the
// player sleeps or fast-travels, and must not stop when the game is paused in a
// menu... which it also does not, because ticks stop then too.
[[nodiscard]] inline double NowSeconds()
{
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

// "Lydia (000A2C94)" -- name plus FormID, because in a test cell you will have
// two Lydias (the real one and a placeatme copy) and the name alone is a lie.
[[nodiscard]] std::string Describe(RE::Actor *actor);

// Just the name, for the UI. Describe() above adds the FormID, which is what a
// log wants and what a screen does not.
[[nodiscard]] std::string DisplayNameOf(RE::Actor *actor);

} // namespace ft::game
