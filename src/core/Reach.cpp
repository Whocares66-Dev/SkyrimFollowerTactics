#include "core/Reach.h"

#include <cmath>

namespace ft
{

float BodyRadius(const Body &body) noexcept
{
    return body.maxY - body.minY > 0.0f ? body.maxY * body.scale : kEmptyBoxRadius;
}

float ReachDistance(const Body &from, const Body &to) noexcept
{
    const float dx = from.x - to.x;
    const float dy = from.y - to.y;
    const float dz = from.z - to.z;
    float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (std::abs(dz) >= kFlatFromHeight)
    {
        const float bottom = to.z + to.minZ;
        const float top = to.z + to.maxZ;
        const auto within = [bottom, top](float z) { return z >= bottom && z <= top; };
        if (within(from.z + from.maxZ) || within(from.z + from.minZ))
            distance = std::hypot(dx, dy);
    }
    return distance - (BodyRadius(from) + BodyRadius(to));
}

} // namespace ft
