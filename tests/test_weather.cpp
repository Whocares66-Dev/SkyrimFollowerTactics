// The weather out of doors from the sky: the class of the weather mostly
// blended in, and the rain or snow that falls through a change of weather
// as the engine's IsRaining and IsSnowing have it. And the part of the day
// by the climate's sun.

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

TEST_CASE("the day in four by Skyrim's climate: sunrise 5:30, sunset 16:00 to 20:30", "[weather][time]")
{
    const SunTimes skyrim;
    REQUIRE(TimeOfDay(0.0f, skyrim) == TimeKind::Night);
    REQUIRE(TimeOfDay(5.4f, skyrim) == TimeKind::Night);
    // Daytime from the start of sunrise to the end of sunset, both in.
    REQUIRE(TimeOfDay(5.5f, skyrim) == TimeKind::Morning);
    REQUIRE(TimeOfDay(11.9f, skyrim) == TimeKind::Morning);
    REQUIRE(TimeOfDay(12.0f, skyrim) == TimeKind::Afternoon);
    REQUIRE(TimeOfDay(15.9f, skyrim) == TimeKind::Afternoon);
    REQUIRE(TimeOfDay(16.0f, skyrim) == TimeKind::Evening);
    REQUIRE(TimeOfDay(20.5f, skyrim) == TimeKind::Evening);
    REQUIRE(TimeOfDay(20.6f, skyrim) == TimeKind::Night);
    REQUIRE(TimeOfDay(23.9f, skyrim) == TimeKind::Night);
}

TEST_CASE("a climate moves the day's edges, noon aside", "[weather][time]")
{
    // Obsidian Weathers' Skyrim: sunset from 15:30.
    const SunTimes obsidian{5.5f, 15.5f, 20.5f};
    REQUIRE(TimeOfDay(15.6f, obsidian) == TimeKind::Evening);
    REQUIRE(TimeOfDay(12.5f, obsidian) == TimeKind::Afternoon);
    // Apocrypha's: sunrise at midnight, sunset from 22:30 to 23:50.
    const SunTimes apocrypha{0.0f, 22.5f, 23.0f + 50.0f / 60.0f};
    REQUIRE(TimeOfDay(0.0f, apocrypha) == TimeKind::Morning);
    REQUIRE(TimeOfDay(13.0f, apocrypha) == TimeKind::Afternoon);
    REQUIRE(TimeOfDay(23.0f, apocrypha) == TimeKind::Evening);
    REQUIRE(TimeOfDay(23.9f, apocrypha) == TimeKind::Night);
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

TEST_CASE("ash falls through its precipitation window, separately from snow", "[weather]")
{
    Sky sky;
    sky.last = Classed(WeatherKind::Cloudy);
    sky.current = Classed(WeatherKind::Ash, 0.2f, 0.6f);
    sky.progress = 0.2f;
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Cloudy));
    sky.progress = 0.3f;
    REQUIRE(WeatherOf(sky) == (Bit(WeatherKind::Cloudy) | Bit(WeatherKind::Ash)));
    sky.progress = 1.0f;
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Ash));

    sky.last = sky.current;
    sky.current = Classed(WeatherKind::Snow, 0.4f);
    sky.progress = 0.4f;
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Ash));
    sky.progress = 0.5f;
    REQUIRE(WeatherOf(sky) == (Bit(WeatherKind::Ash) | Bit(WeatherKind::Snow)));
    sky.progress = 0.6f;
    REQUIRE(WeatherOf(sky) == Bit(WeatherKind::Snow));
}

TEST_CASE("under a roof it is dark below the engine's torch line, by the actor's own light", "[brightness]")
{
    const TorchLight torch;
    LightRead light;
    light.where = LightRead::Where::Roofed;
    light.level = 12.0f;
    REQUIRE(BrightnessOf(light, torch) == Bit(BrightnessKind::Dark));
    light.level = 39.9f;
    REQUIRE(BrightnessOf(light, torch) == Bit(BrightnessKind::Dark));
    // At the line it is no longer dark, and never both.
    light.level = 40.0f;
    REQUIRE(BrightnessOf(light, torch) == Bit(BrightnessKind::Bright));
    light.level = 260.0f;
    REQUIRE(BrightnessOf(light, torch) == Bit(BrightnessKind::Bright));
    // The sky's ambient and the hour are not asked under a roof.
    light.level = 12.0f;
    light.ambient = 3.0f;
    light.hour = 12.0f;
    REQUIRE(BrightnessOf(light, torch) == Bit(BrightnessKind::Dark));

    // A setting above the engine's cap: the level counts to 100 at most, so
    // everything is dark, as the engine's own test has it.
    TorchLight high;
    high.roofed = 120.0f;
    light.level = 260.0f;
    REQUIRE(BrightnessOf(light, high) == Bit(BrightnessKind::Dark));
}

TEST_CASE("under the sky it is dark by the sky's ambient, against the night's line or the day's", "[brightness]")
{
    const TorchLight torch;
    LightRead light;
    light.where = LightRead::Where::UnderSky;
    // The actor's own level is not asked: the engine keeps none out here.
    light.level = 0.0f;

    // 20:00 to 06:00 is the night's line, 1.2; the rest the day's, 0.6.
    light.ambient = 1.0f;
    light.hour = 23.0f;
    REQUIRE(BrightnessOf(light, torch) == Bit(BrightnessKind::Dark));
    light.hour = 3.0f;
    REQUIRE(BrightnessOf(light, torch) == Bit(BrightnessKind::Dark));
    light.hour = 12.0f;
    REQUIRE(BrightnessOf(light, torch) == Bit(BrightnessKind::Bright));
    // The hours themselves are the day's.
    light.hour = 20.0f;
    REQUIRE(BrightnessOf(light, torch) == Bit(BrightnessKind::Bright));
    light.hour = 6.0f;
    REQUIRE(BrightnessOf(light, torch) == Bit(BrightnessKind::Bright));

    // A black storm at noon is dark; a bright night is not.
    light.hour = 12.0f;
    light.ambient = 0.5f;
    REQUIRE(BrightnessOf(light, torch) == Bit(BrightnessKind::Dark));
    light.hour = 23.0f;
    light.ambient = 1.2f;
    REQUIRE(BrightnessOf(light, torch) == Bit(BrightnessKind::Bright));
}

TEST_CASE("with nothing read it is neither dark nor bright", "[brightness]")
{
    REQUIRE(BrightnessOf(LightRead{}, TorchLight{}) == 0);
}
