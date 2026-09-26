#pragma once
// The weather over an actor, for the Weather condition (dev/CONDITIONS.md
// 2d), from the sky as the engine keeps it: the weather coming in, the one
// going out, and how far the change between them has gone. The game reads
// the sky and says whether the actor is out of doors (game/Places.cpp,
// ReadWeather); what that makes the weather is here, where the changes of
// weather are tested. No Skyrim.

#include "core/Kinds.h"

#include <cstdint>
#include <optional>

namespace ft
{

// One weather record as the sky blends it: the classes it carries, a bit
// per WeatherKind, and where in a change of weather its rain or snow
// begins to fall when it comes in and stops when it goes out, each a share
// of the change.
struct SkyWeather
{
    std::uint8_t classes{0};
    float precipitationBegins{0.0f};
    float precipitationEnds{0.0f};
};

struct Sky
{
    std::optional<SkyWeather> current;
    std::optional<SkyWeather> last;
    // 1 once the change is complete.
    float progress{1.0f};
};

// A bit per WeatherKind. Pleasant and Cloudy are the class of the weather
// at least half blended in. Rain and Snow are whether it falls, as the
// engine's IsRaining and IsSnowing conditions have it (1.6.1170,
// 2026-09-26): from the incoming weather once the change passes its
// begin, and from the outgoing one until the change reaches its end.
[[nodiscard]] std::uint8_t WeatherOf(const Sky &sky) noexcept;

} // namespace ft
