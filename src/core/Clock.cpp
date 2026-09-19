#include "core/Clock.h"

namespace ft
{

double GameClock::Sample(double hour, double timescale) noexcept
{
    if (lastHour_ >= 0.0)
    {
        double deltaHours = hour - lastHour_;
        if (deltaHours < 0.0)
            deltaHours += 24.0;
        const double scale = timescale > 0.0 ? timescale : kDefaultTimescale;
        seconds_ += deltaHours * 3600.0 / scale;
    }
    lastHour_ = hour;
    return seconds_;
}

} // namespace ft
