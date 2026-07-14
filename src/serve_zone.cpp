#include "serve_zone.hpp"

using namespace DirectX;

ServeZone::ServeZone(FXMVECTOR origin, float radius) : radius_(radius) {
    XMStoreFloat3(&origin_, origin);
}

bool ServeZone::Contains(FXMVECTOR point) const {
    XMFLOAT3 p;
    XMStoreFloat3(&p, point);
    const float dx = p.x - origin_.x;
    const float dz = p.z - origin_.z;
    return dx * dx + dz * dz <= radius_ * radius_;
}
