#pragma once

#include <DirectXMath.h>

// FMOD Core types, forward-declared so fmod.hpp stays out of this header and off
// the rest of the game's include graph. Only pointers to these are held below.
namespace FMOD {
class System;
class Sound;
class Channel;
} // namespace FMOD

// Which clip PlayImpact sounds. Defined in full in rigid_body.hpp, where the
// physics tags bodies with it; forward-declared here (with its fixed underlying
// type) so this header need not pull that one in.
enum class ImpactSound : unsigned char;

// The backyard's sound. The grill sizzles from its own spot in the world, so it
// swells as the player walks up to it and pans as they turn; and one-shot impacts
// sound when the physics reports a body landing on something -- a splat for meat,
// a clank for the tongs. FMOD does the mixing on its own thread; this class
// re-poses the listener each frame, fires the impacts the loop hands it, and lets
// the engine tidy up after itself.
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

    // Fires a one-shot at `position` in the world, for a body the physics reported
    // landing on something -- `sound` picks the clip (a meat splat or a metal
    // clank). `strength` is the contact impulse the solver measured; it is mapped
    // to a volume, so a hurled patty cracks louder than one set down. A fresh FMOD
    // voice is taken each call and freed by the engine when the clip ends, so
    // overlapping impacts layer rather than cut off. A no-op until the listener has
    // been posed (the first Update), if the engine never came up, or if the clip
    // this sound needs failed to load.
    void PlayImpact(const DirectX::XMFLOAT3& position, float strength, ImpactSound sound);

private:
    FMOD::System* system_ = nullptr;
    FMOD::Sound* sizzle_ = nullptr;
    FMOD::Sound* splat_ = nullptr;
    FMOD::Sound* clank_ = nullptr;
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
