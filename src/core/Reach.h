#pragma once
// How far one actor is from another as the engine measures a blow's
// reach (47273 on 1.6.1170, dev/ACTIONS.md 6): centre to centre, flat when
// their heights differ by 48 or more and either end of the attacker's
// bounds lies within the target's, less both bodies' radii. The game reads
// each actor's position, bounds and scale (game/Blows.cpp,
// ReachDistance); the measure is here, where its thresholds are tested.
// Not mirrored: a pair of flags on the two actors that has the engine test
// the overlap at any difference in height; under 48 the flat and full
// distances differ by a few units. No Skyrim.

namespace ft
{

// One actor as the measure reads it.
struct Body
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    // The bounds, relative to the position: the box's y extent gives the
    // radius, its z extent the height the overlap is tested over.
    float minY{0.0f};
    float maxY{0.0f};
    float minZ{0.0f};
    float maxZ{0.0f};
    float scale{1.0f};
};

// The body's radius: the box's far y edge at the actor's scale, or 16 for
// a box with no extent.
[[nodiscard]] float BodyRadius(const Body &body) noexcept;

// Heights this far apart or more are measured flat, where the ends
// overlap.
inline constexpr float kFlatFromHeight = 48.0f;
inline constexpr float kEmptyBoxRadius = 16.0f;

[[nodiscard]] float ReachDistance(const Body &from, const Body &to) noexcept;

} // namespace ft
