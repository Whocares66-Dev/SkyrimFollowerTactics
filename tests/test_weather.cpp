// The weather out of doors from the sky: the class of the weather mostly
// blended in, and the rain or snow that falls through a change of weather
// as the engine's IsRaining and IsSnowing have it.

#include <catch2/catch_test_macros.hpp>

#include "core/Weather.h"

using namespace ft;

namespace
{

SkyWeather Classed(WeatherKind kind, float begins = 0.0f, float ends = 0.0f)
{
    return {Bit(kind), begins, ends};
}

bool Has(std::uint8_t weather, WeatherKind kind)
{
    return (weather & Bit(kind)) != 0;
}

} // namespace

TEST_CASE("a settled sky is its weather's class", "[weather]")
{
    Sky sky;
    sky.current = Classed(WeatherKind::Pleasant);
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Pleasant));
    sky.current = Classed(WeatherKind::Cloudy);
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Cloudy));
    // Rain once the change is done, whatever its begin.
    sky.current = Classed(WeatherKind::Rain, 0.8f, 1.0f);
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Rain));
    sky.current = Classed(WeatherKind::Snow, 0.3f, 0.5f);
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Snow));
    // A weather of no class, the game's lighting effects: none.
    sky.current = SkyWeather{};
    REQUIRE(WeatherOf(sky) == 0);
    // No weather at all, as after the sky is reset.
    REQUIRE(WeatherOf(Sky{}) == 0);
}

TEST_CASE("through a change the sky is the weather at least half in", "[weather]")
{
    Sky sky;
    sky.last = Classed(WeatherKind::Pleasant);
    sky.current = Classed(WeatherKind::Cloudy);
    sky.progress = 0.2f;
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Pleasant));
    sky.progress = 0.5f;
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Cloudy));
    sky.progress = 0.9f;
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Cloudy));
}

TEST_CASE("rain falls once the change passes the incoming weather's begin", "[weather]")
{
    Sky sky;
    sky.last = Classed(WeatherKind::Cloudy);
    sky.current = Classed(WeatherKind::Rain, 0.3f, 0.6f);
    sky.progress = 0.3f;
    REQUIRE_FALSE(Has(WeatherOf(sky), WeatherKind::Rain));
    // Falling while the overcast sky still has the most of it: both.
    sky.progress = 0.4f;
    REQUIRE(WeatherOf(sky) == (Bit(WeatherKind::Cloudy) | Bit(WeatherKind::Rain)));
    sky.progress = 0.7f;
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Rain));
}

TEST_CASE("rain or snow stops once the change reaches the outgoing weather's end", "[weather]")
{
    Sky sky;
    sky.last = Classed(WeatherKind::Snow, 0.2f, 0.6f);
    sky.current = Classed(WeatherKind::Pleasant);
    sky.progress = 0.4f;
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Snow));
    // Still snowing under a sky mostly clear.
    sky.progress = 0.55f;
    REQUIRE(WeatherOf(sky) == (Bit(WeatherKind::Pleasant) | Bit(WeatherKind::Snow)));
    sky.progress = 0.6f;
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Pleasant));
}

TEST_CASE("rain turning to snow can fall as both for a while", "[weather]")
{
    Sky sky;
    sky.last = Classed(WeatherKind::Rain, 0.2f, 0.7f);
    sky.current = Classed(WeatherKind::Snow, 0.4f, 0.8f);
    sky.progress = 0.3f;
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Rain));
    sky.progress = 0.5f;
    REQUIRE(WeatherOf(sky) == (Bit(WeatherKind::Rain) | Bit(WeatherKind::Snow)));
    sky.progress = 0.75f;
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Snow));
}
