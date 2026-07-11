#pragma once

#include <DirectXMath.h>

// FMOD Core types, forward-declared so fmod.hpp stays out of this header and off
// the rest of the game's include graph. Only pointers to these are held below.
namespace FMOD {
class System;
class Sound;
class Channel;
} // namespace FMOD

// The backyard's sound. For now that is one thing: the grill, sizzling from its
// own spot in the world, so it swells as the player walks up to it and pans left
// or right as they turn. FMOD does the mixing on its own thread; this class only
// re-poses the listener each frame and lets the engine tidy up after itself.
class Audio {
public:
    Audio();
    ~Audio();

    // Owns an FMOD::System and a loaded sound; copying would double-free them.
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    // Advances the audio engine and re-solves the listener relative to the grill.
    // `camera_to_world` is the same matrix the viewmodel is posed with: its rows
    // are the listener's right, up and forward, and its translation is the ear.
    // `dt` is the frame time the loop has already clamped.
    void Update(DirectX::FXMMATRIX camera_to_world, float dt);

private:
    FMOD::System* system_ = nullptr;
    FMOD::Sound* sizzle_ = nullptr;
    FMOD::Channel* channel_ = nullptr;

    // Set once the engine hits an error it will not come back from -- no audio
    // hardware at all, say, or a failed load. The game then runs on in silence
    // rather than paying to re-solve 3D audio every frame for nothing.
    bool silent_ = false;

    // The loop is not started in the constructor: at that point there is no
    // listener yet, so the voice would sound for a frame or two at full,
    // un-attenuated gain from no particular direction -- a blast from everywhere
    // on load. Instead the first Update poses the listener, starts the voice
    // paused with the emitter already placed, then unpauses it -- so the sizzle
    // is already distance-shaped before its first sample is heard.
    bool started_ = false;
};
