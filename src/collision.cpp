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
