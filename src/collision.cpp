#include "collision.hpp"

#include <cfloat>
#include <initializer_list>

using namespace DirectX;

Aabb TransformBounds(const Aabb& bounds, FXMMATRIX transform) {
    XMVECTOR minimum = XMVectorReplicate(FLT_MAX);
    XMVECTOR maximum = XMVectorReplicate(-FLT_MAX);

    for (const float x : {bounds.min.x, bounds.max.x}) {
        for (const float y : {bounds.min.y, bounds.max.y}) {
            for (const float z : {bounds.min.z, bounds.max.z}) {
                const XMVECTOR corner = XMVector3Transform(XMVectorSet(x, y, z, 1.0f), transform);
                minimum = XMVectorMin(minimum, corner);
                maximum = XMVectorMax(maximum, corner);
            }
        }
    }

    Aabb result{};
    XMStoreFloat3(&result.min, minimum);
    XMStoreFloat3(&result.max, maximum);
    return result;
}

OrientedBox TransformBox(const Aabb& bounds, FXMMATRIX transform) {
    const XMVECTOR box_min = XMLoadFloat3(&bounds.min);
    const XMVECTOR box_max = XMLoadFloat3(&bounds.max);

    OrientedBox box{};

    XMVECTOR scale;
    XMVECTOR rotation;
    XMVECTOR translation;
    if (XMMatrixDecompose(&scale, &rotation, &translation, transform)) {
        // A clean scale/rotate/translate: the box centre is the local centre
        // carried all the way into the world, and its half-extents are the local
        // half-sizes scaled, then oriented by the rotation the decomposition pulled
        // out. XMVectorAbs guards the (unused here) mirrored case from a negative
        // extent.
        const XMVECTOR local_center = XMVectorScale(XMVectorAdd(box_min, box_max), 0.5f);
        const XMVECTOR local_half = XMVectorScale(XMVectorSubtract(box_max, box_min), 0.5f);
        XMStoreFloat3(&box.center, XMVector3Transform(local_center, transform));
        XMStoreFloat3(&box.half_extents, XMVectorMultiply(local_half, XMVectorAbs(scale)));
        XMStoreFloat4(&box.orientation, rotation);
        return box;
    }

    // Sheared beyond a box: keep the old loose axis-aligned bound, unrotated.
    const Aabb aabb = TransformBounds(bounds, transform);
    const XMVECTOR aabb_min = XMLoadFloat3(&aabb.min);
    const XMVECTOR aabb_max = XMLoadFloat3(&aabb.max);
    XMStoreFloat3(&box.center, XMVectorScale(XMVectorAdd(aabb_min, aabb_max), 0.5f));
    XMStoreFloat3(&box.half_extents, XMVectorScale(XMVectorSubtract(aabb_max, aabb_min), 0.5f));
    box.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    return box;
}
