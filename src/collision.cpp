#include "collision.hpp"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace {

// Boxes whose top is below this are walked over rather than climbed. The eye is
// pinned to a constant height, so the player has no legs to lift over a curb --
// the ground and the patio slab would otherwise block every step.
constexpr float kStepHeight = 0.25f;

// Below this the cylinder's centre is treated as being *inside* the box, where
// the direction away from the nearest surface point is numerically meaningless.
constexpr float kInsideEpsilon = 1e-4f;

// Two boxes meeting at a corner can each push the player into the other. Four
// passes settle every arrangement in this scene; a stubborn one just stops.
constexpr int kResolvePasses = 4;

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
            const float nearest_x = std::clamp(eye.x, box.min.x, box.max.x);
            const float nearest_z = std::clamp(eye.z, box.min.z, box.max.z);
            const float dx = eye.x - nearest_x;
            const float dz = eye.z - nearest_z;
            const float distance_squared = dx * dx + dz * dz;

            if (distance_squared >= radius * radius) {
                continue;
            }

            if (distance_squared > kInsideEpsilon) {
                // Outside the box but touching it: back off along the surface
                // normal, which lets the player slide along a wall.
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
