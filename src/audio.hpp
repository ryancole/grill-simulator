#pragma once

#include <Audio.h>

#include <DirectXMath.h>

#include <memory>

// The backyard's sound. For now that is one thing: the grill, sizzling from its
// own spot in the world, so it swells as the player walks up to it and pans left
// or right as they turn. XAudio2 does the mixing on its own thread; this class
// only reloads the 3D panning each frame and lets the engine tidy up after
// itself.
class Audio {
public:
    Audio();

    // Advances the audio engine and re-solves the grill's position relative to
    // the listener. `camera_to_world` is the same matrix the viewmodel is posed
    // with: its rows are the listener's right, up and forward, and its
    // translation is the ear. `dt` is the frame time the loop has already clamped.
    void Update(DirectX::FXMMATRIX camera_to_world, float dt);

private:
    std::unique_ptr<DirectX::AudioEngine> engine_;
    std::unique_ptr<DirectX::SoundEffect> sizzle_;
    std::unique_ptr<DirectX::SoundEffectInstance> sizzle_instance_;
    DirectX::AudioListener listener_;
    DirectX::AudioEmitter emitter_;

    // Set once the engine hits an error it will not come back from -- no audio
    // hardware at all, say. The game then runs on in silence rather than paying
    // to re-solve 3D audio for a device that is never coming back.
    bool silent_ = false;
};
