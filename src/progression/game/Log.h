#pragma once
// Progression's modules of Tactics' log (game/Log.h): the same
// FollowerTactics.log, at the level FollowerTactics.ini sets, each line naming
// the part that wrote it:
//
//   [20:41:07.312] [info] [growth] Lydia's One-Handed increased to 26

#include "game/Log.h"

namespace fp::log
{

using ft::log::Module;

inline constexpr Module plugin{"progression"};
inline constexpr Module party{"party"};
inline constexpr Module growth{"growth"};
inline constexpr Module perks{"perks"};
inline constexpr Module spells{"spells"};
inline constexpr Module save{"ledger"};
inline constexpr Module ui{"progression-ui"};

} // namespace fp::log
