#pragma once
// Small helpers for the game layer. Deliberately minimal.
//
// The temptation with an engine adapter is to build a utility layer before you
// know what actually repeats. These earn their place because every
// diagnostic line needs to name an actor, and every cooldown needs a clock.
// Anything else stays inline until it has repeated three times.

#include <chrono>
#include <cstdint>
#include <ranges>
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
// A reading of the calendar's hour and timescale, handed to core's
// GameClock (core/Clock.h), which says why game time and what the hour
// alone cannot tell. Any thread.
[[nodiscard]] double TacticsSeconds();

// "Lydia (000A2C94)" -- name plus FormID, because in a test cell you will have
// two Lydias (the real one and a placeatme copy) and the name alone is a lie.
[[nodiscard]] std::string Describe(RE::Actor *actor);

// Just the name, for the UI. Describe() above adds the FormID, which is what a
// log wants and what a screen does not.
[[nodiscard]] std::string DisplayNameOf(RE::Actor *actor);

// The effects of a magic item that name their base effect: the only ones
// anything here can read, since every reader goes on to the base. A record
// can point at an MGEF the load order does not define -- a removed or merged
// mod's leavings -- and that effect arrives with no base. Checked here, once,
// rather than at the top of every loop; the few walks that must still see
// such an effect say so where they stand.
[[nodiscard]] inline auto ResolvedEffects(const RE::MagicItem &item)
{
    return item.effects | std::views::filter([](const RE::Effect *effect) { return effect && effect->baseEffect; });
}

// A game setting by name, looked up on every call (a mod may change one
// mid-session), or `vanilla` where the game has no such setting.
[[nodiscard]] float GameSetting(const char *name, float vanilla);
[[nodiscard]] std::int32_t GameSetting(const char *name, std::int32_t vanilla);

} // namespace ft::game
