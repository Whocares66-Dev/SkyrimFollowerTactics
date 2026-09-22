#pragma once
// The paced tick that watches the party (progression/game/Service.h does the rest), the
// player's level-ups, and the main menu's opening, which ends the game the
// panel acts on. A
// companion's skill use comes from progression/game/Learning.h.

namespace fp::game
{

// At data load: the sinks, and the thread that paces the tick.
void InstallEvents();

} // namespace fp::game
