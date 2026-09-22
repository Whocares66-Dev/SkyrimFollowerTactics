#pragma once
// Learning a spell from a tome and forgetting one (dev/PROGRESSION.md,
// "Spells"): the spell as the ledger keeps it, and a button's words. What
// either does to a companion is progression/core/Companion.h's (ReadTome,
// ForgetSpell). Whether they know the spell is the engine's answer, asked
// through the spell view (progression/game/SpellView.h), so one of theirs
// set aside reads as not known and one taught here as known. No Skyrim.

#include "progression/core/Ids.h"

#include <string>
#include <string_view>

namespace fp
{

struct SpellFacts
{
    FormKey spell;
    std::string name; // kept for a form gone missing
};

// Whether a click can act, and what it does or why it cannot, in the
// panel's words.
struct SpellButton
{
    bool can{false};
    std::string hover;
};

// Learn, on a tome's page: as the player reads one, nothing is asked but
// that they do not know the spell already.
[[nodiscard]] SpellButton LearnButton(std::string_view spell, bool known);
// Forget, on a spell's page: any spell they know.
[[nodiscard]] SpellButton ForgetButton(std::string_view spell);

} // namespace fp
