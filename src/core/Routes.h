#pragma once
// How an action is performed, by who performs it: the one place that says
// what the player can do and what a follower can, and how each does it.
// The game performs the route (game/Actions.cpp, Execute); the capabilities
// and the panel's menu offer what has one. A policy -- the strongest, the
// weakest, any -- is resolved to one thing by the evaluator before this,
// so the kinds that differ only in how the thing was chosen share a route.
// No Skyrim.

#include "Rule.h"

#include <cstdint>

namespace ft
{

enum class Performer : std::uint8_t
{
    Follower,
    Player
};

enum class Route : std::uint8_t
{
    None,        // no way for this performer
    Consume,     // a potion, a food or an ingredient, by the equip the game consumes through
    ApplyPoison, // onto the weapon in hand
    Charge,      // a soul gem into the weapon in hand
    CastRecord,  // a follower's spell or scroll: their UseMagic record
    CastPress,   // the player's: a press of the hand's control
    VoiceRecord, // a follower's power or shout: their Shout record
    VoicePress,  // the player's: a press of the shout control
    Pin,         // a follower's equip, kept against their AI
    Wear,        // the player's: a plain equip
    Target,      // a follower's Attack: their combat target; the player aims for themself
    Bash,        // a bash or a power bash, from the block
    Strike       // a power attack, as the combat AI makes one
};

[[nodiscard]] Route RouteOf(ActionKind kind, Performer performer) noexcept;

} // namespace ft
