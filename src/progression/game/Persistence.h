#pragma once
// Progression's records in the SKSE co-save. They go in Tactics' block, the
// one the plugin's id names, after Tactics' own records, and are read out of
// the same pass by their types (game/Profiles.cpp): SKSE keeps one block per
// plugin and hands it back with the save it belongs to, so a companion's
// progression is always that of the save being played -- loading an
// earlier save rolls it back with everything else. The framing is core's
// (progression/core/Serialize.h).

#include "progression/core/Serialize.h"

#include <vector>

namespace fp::game
{

// SKSE's save callback, after Tactics' records.
void WriteRecords(SKSE::SerializationInterface *intfc);

// SKSE's load callback: Progression's records, each read whole.
void ReadRecords(const std::vector<CoSaveRecord> &records);

// SKSE's revert callback.
void RevertRecords();

} // namespace fp::game
