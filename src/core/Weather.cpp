#include "core/Weather.h"

namespace ft
{

std::uint8_t WeatherOf(const Sky &sky) noexcept
{
    if (!sky.current)
        return 0;
    const SkyWeather &seen = sky.last && sky.progress < 0.5f ? *sky.last : *sky.current;
    std::uint8_t out = seen.classes & (Bit(WeatherKind::Pleasant) | Bit(WeatherKind::Cloudy));
    for (const WeatherKind falling : {WeatherKind::Rain, WeatherKind::Snow, WeatherKind::Ash})
    {
        const bool coming =
            (sky.current->classes & Bit(falling)) != 0 && sky.progress > sky.current->precipitationBegins;
        const bool going =
            sky.last && (sky.last->classes & Bit(falling)) != 0 && sky.progress < sky.last->precipitationEnds;
        if (coming || going)
            out |= Bit(falling);
    }
    return out;
}

} // namespace ft
