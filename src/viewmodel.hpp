#pragma once

#include "scene.hpp"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <vector>

// Everything the renderer needs to draw the arms for one frame.
struct ViewmodelPose {
    std::span<const MeshInstance> instances;
    // World space, unit length. Not the yard's sun -- see Viewmodel::Pose.
    DirectX::XMFLOAT3 sun_direction;
};

// The player's own arms, hanging in the bottom corners of the screen.
//
// The segments are authored once in eye space -- the origin is the eye, +X is
// right, +Y is up, +Z is the way the player is looking -- and multiplied into
// the world by the camera's own basis every frame. Doing it that way rather than
// leaving them in eye space keeps `g_model` a genuine world matrix, so the arms
// take the same ambient and the same hemisphere as everything else in the yard.
//
// They are boxes, like the yard's props, and share the scene's unit cube.
class Viewmodel {
public:
    explicit Viewmodel(std::uint32_t cube_model);

    // Lifts the arms into the world for one frame. The span stays valid until
    // the next call.
    ViewmodelPose Pose(const DirectX::XMMATRIX& camera_to_world);

private:
    // One box spanning `start` to `end`, `thickness` wide and tall across it.
    void AddLimb(DirectX::XMFLOAT3 start, DirectX::XMFLOAT3 end, DirectX::XMFLOAT2 thickness,
                 DirectX::XMFLOAT3 color);
    // `side` is +1 for the right arm and -1 for the left, which is the same arm
    // with its eye-space X negated.
    void AddArm(float side);

    std::uint32_t cube_;
    std::vector<MeshInstance> eye_space_;
    std::vector<MeshInstance> world_space_;
};
