#pragma once

#include "collision.hpp"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

struct Vertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
};

// Every prop is the same unit cube, scaled and rotated into place, so the
// renderer uploads one mesh and issues one draw per prop.
struct Prop {
    DirectX::XMFLOAT4X4 transform;
    DirectX::XMFLOAT3 color;
    // Size in metres of one checkerboard tile, projected down the Y axis. Zero
    // leaves the surface flat -- it is only there to give the ground a texture
    // to move against.
    float checker;
};

// A backyard, assembled entirely out of boxes.
class Scene {
public:
    Scene();

    const std::vector<Vertex>& Vertices() const { return vertices_; }
    const std::vector<std::uint16_t>& Indices() const { return indices_; }
    const std::vector<Prop>& Props() const { return props_; }
    const std::vector<Aabb>& Colliders() const { return colliders_; }

private:
    void BuildUnitCube();
    void AddBox(DirectX::XMFLOAT3 center, DirectX::XMFLOAT3 size, float yaw_degrees,
                DirectX::XMFLOAT3 color, float checker = 0.0f);

    std::vector<Vertex> vertices_;
    std::vector<std::uint16_t> indices_;
    std::vector<Prop> props_;
    std::vector<Aabb> colliders_;
};
