#include "collision.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace DirectX;

namespace {

// Below this the cylinder's centre is treated as being *inside* the box, where
// the direction away from the nearest surface point is numerically meaningless.
constexpr float kInsideEpsilon = 1e-4f;

// Two boxes meeting at a corner can each push the player into the other. Four
// passes settle every arrangement in this scene; a stubborn one just stops.
constexpr int kResolvePasses = 4;

// Squared distance in the ground plane from a point to the box nearest it. Zero
// when the point is directly over the box.
float DistanceSquaredToBoxXZ(float x, float z, const Aabb& box) {
    const float dx = x - std::clamp(x, box.min.x, box.max.x);
    const float dz = z - std::clamp(z, box.min.z, box.max.z);
    return dx * dx + dz * dz;
}

} // namespace

XMFLOAT3 ResolveCollision(XMFLOAT3 eye, float radius, float eye_height,
                          std::span<const Aabb> boxes) {
    const float feet = eye.y - eye_height;
    const float head = eye.y;

    for (int pass = 0; pass < kResolvePasses; ++pass) {
        bool pushed = false;

        for (const Aabb& box : boxes) {
            // Short enough to step over, or high enough to duck under.
            if (box.max.y <= feet + kStepHeight || box.min.y >= head) {
                continue;
            }

            // Overlap is decided in the ground plane against the point on the
            // box closest to the player, which handles faces and corners alike.
            const float distance_squared = DistanceSquaredToBoxXZ(eye.x, eye.z, box);
            if (distance_squared >= radius * radius) {
                continue;
            }

            if (distance_squared > kInsideEpsilon) {
                // Outside the box but touching it: back off along the surface
                // normal, which lets the player slide along a wall.
                const float dx = eye.x - std::clamp(eye.x, box.min.x, box.max.x);
                const float dz = eye.z - std::clamp(eye.z, box.min.z, box.max.z);
                const float distance = std::sqrt(distance_squared);
                const float push = radius - distance;
                eye.x += dx / distance * push;
                eye.z += dz / distance * push;
            } else {
                // Inside the box: leave through the nearest wall.
                const float west = eye.x - box.min.x;
                const float east = box.max.x - eye.x;
                const float south = eye.z - box.min.z;
                const float north = box.max.z - eye.z;
                const float least = std::min({west, east, south, north});

                if (least == west) {
                    eye.x = box.min.x - radius;
                } else if (least == east) {
                    eye.x = box.max.x + radius;
                } else if (least == south) {
                    eye.z = box.min.z - radius;
                } else {
                    eye.z = box.max.z + radius;
                }
            }

            pushed = true;
        }

        if (!pushed) {
            break;
        }
    }

    return eye;
}

float HighestSupportUnder(float x, float z, float radius, float ceiling,
                          std::span<const Aabb> boxes) {
    float highest = -FLT_MAX;

    for (const Aabb& box : boxes) {
        // A surface out of reach overhead is a ceiling, not a floor. Note that
        // the whole box is judged by its top: the player stands on the roof of
        // the picnic table, never inside it, because the legs block them first.
        if (box.max.y > ceiling || box.max.y <= highest) {
            continue;
        }
        if (DistanceSquaredToBoxXZ(x, z, box) >= radius * radius) {
            continue;
        }
        highest = box.max.y;
    }

    return highest;
}
