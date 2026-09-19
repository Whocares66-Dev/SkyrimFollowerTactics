#pragma once
// The tactics clock: game time, in real seconds at the current timescale,
// from readings of the engine's hour-of-day global. Every cooldown, lease
// and hit window is measured on it. The game side reads the calendar and
// hands the numbers here (game/Util.cpp, TacticsSeconds); what is made of
// them is decided here, where it is tested. No Skyrim.
//
// Game time is the clock the game paces things by, and it has every
// property a tactic wants: it stops in menus and while the world is
// frozen, so a request armed just before the panel opened is exactly as
// old when it closes; and it jumps on wait, sleep and fast travel, which
// expires every cooldown, also wanted. Timescale (20 by default) is divided
// out at each step, so "2 s" means two real seconds of play whatever the
// timescale.
//
// Read from the hour of day, not "hours passed": the latter is a float that
// loses sub-second precision after a few hundred game days, the former stays
// within 0..24 and precise. The cost is that a day is not told from no
// time at all, and an hour gone backwards reads as midnight crossed: a
// second sample of 8:00 after 20:00 counts twelve hours, a whole day
// slept counts nothing. Both are rare, both only expire or withhold a
// cooldown, and the tests below say so rather than hide it.

namespace ft
{

class GameClock
{
  public:
    // The timescale taken when the calendar's is not positive: the game's
    // own default.
    static constexpr double kDefaultTimescale = 20.0;

    // A reading of the calendar: the hour of day, in [0, 24), and the
    // timescale, game hours per real hour. Returns the seconds so far. The
    // first reading counts nothing; each later one adds the hours since
    // the last, a smaller hour taken as midnight crossed.
    double Sample(double hour, double timescale) noexcept;

    [[nodiscard]] double Seconds() const noexcept
    {
        return seconds_;
    }

  private:
    double lastHour_{-1.0}; // -1: no reading yet
    double seconds_{0.0};
};

} // namespace ft
