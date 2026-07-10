#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// A decoded image and its mip chain, ready to be handed to Direct3D. Level 0 is
// the full resolution; the last level is 1x1. Every level is tightly packed
// 8-bit RGBA -- no row padding, so a level's pitch is always width * 4.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::vector<std::byte>> levels;
};

// Decodes one PNG or JPEG -- the two formats glTF is allowed to embed -- into
// RGBA and builds its mip chain.
//
// Decoding goes through the Windows Imaging Component, which ships with the OS,
// so the game gains a texture pipeline without gaining a dependency. COM must
// already be initialised on the calling thread.
Image DecodeImage(std::span<const std::byte> encoded);
