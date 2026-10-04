#pragma once
// Where an actor is, for the Location condition (dev/CONDITIONS.md 2c):
// inside or out by the cell, the rest by the keywords of the location they
// are in and the ones it lies in, and the hold. And the weather over them
// out of doors, for the Weather condition (2d), the part of the day, for
// the Time condition (2e), and how bright it is where they stand, for the
// Brightness condition (2f).

#include "core/Snapshot.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
class Actor;
} // namespace RE

namespace ft::game
{

// The actor's places, hold, weather, part of the day and brightness, into
// the snapshot.
void ReadPlaces(RE::Actor *actor, ft::Snapshot &s);

// The sun as the sky times it now: its climate's, or the last climate's
// when it has none; Skyrim's own before there is a sky.
[[nodiscard]] ft::SunTimes SunNow();

// The holds of the load order, as the Hold heading lists them: every
// location marked a hold, by the game's name for it.
struct HoldPick
{
    std::uint32_t form{0};
    std::string name;
};
[[nodiscard]] const std::vector<HoldPick> &Holds();

// A hold's name as the panel shows it: the game's, begun with a capital.
// Skyrim.esm names three "the Pale", "the Reach", "the Rift", for the
// middle of a sentence; USSEP capitalises them, and a load order without
// it would list them so.
[[nodiscard]] std::string HoldName(std::uint32_t form);

// A load or a new game: the places last read of every actor forgotten, so
// the debug line that says when they change starts afresh, as the
// statuses' does.
void ForgetPlaces();

} // namespace ft::game
