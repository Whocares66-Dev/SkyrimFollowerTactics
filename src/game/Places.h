#pragma once
// Where an actor is, for the Location condition (dev/CONDITIONS.md 2c):
// inside or out by the cell, the rest by the keywords of the location they
// are in and the ones it lies in, and the hold.

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

// The actor's places and hold, into the snapshot.
void ReadPlaces(RE::Actor *actor, ft::Snapshot &s);

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
