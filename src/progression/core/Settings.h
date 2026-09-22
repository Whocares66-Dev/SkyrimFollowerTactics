#pragma once
// The player's choices on the Settings page, kept in the save. The rules
// themselves are the game's, read live (progression/core/Levelling.h). No Skyrim.

namespace fp
{

struct Settings
{
    bool autoEnroll{true};

    // On the HUD: a companion's level-up and what it brings; each skill-up,
    // as the player's own are shown.
    bool notifyLevels{true};
    bool notifySkills{false};

    // Offer perks the catalog says do nothing for a companion, and the
    // trees of skills companions are not trained in.
    bool showNoEffectPerks{false};

    // Levelling off: Tactics' Settings page, "Enable leveling for followers".
    // Kept with the save, so loading it does not put everything back. While
    // set, nothing of ours is on any companion; the ledger is kept for when
    // it is turned on again.
    bool released{false};

    bool operator==(const Settings &) const = default;
};

} // namespace fp
