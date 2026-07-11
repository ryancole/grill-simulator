#include "audio.hpp"

#include "dx_common.hpp"

#include <fmod.hpp>
#include <fmod_errors.h>

#include <cstddef>
#include <filesystem>
#include <string>

using namespace DirectX;

namespace {

// The grill instance stands at (0, 0, 5) in scene.cpp; this lifts the emitter to
// roughly grate height, so the sizzle comes off the cooking surface rather than
// the floor. There is only the one sound, so it is kept in step with the scene by
// hand rather than plumbed through from it. The emitter never moves, so it is
// posed once when the voice starts rather than every frame.
constexpr FMOD_VECTOR kGrillEmitter{0.0f, 0.9f, 5.0f};

// The one global volume knob, applied to the master channel group so it scales
// every sound at once. 1.0 is unity (the mix as authored); below that turns the
// whole game down, above that amplifies it. This is the dial to turn for overall
// loudness -- the per-emitter curve below only shapes falloff with distance.
constexpr float kMasterVolume = 0.5f;

// How far, in metres, the sizzle carries -- the distance of the curve's last
// point below, past which its final value (silence) is held. The player spawns
// ~12 m from the grill; they have to be well onto the patio, close to the grill,
// before it is audible at all.
constexpr float kAudibleRadius = 7.0f;

// The distance-to-volume curve, from the grate (distance 0, full gain) out to the
// radius (silent). FMOD interpolates linearly between the points and holds the
// last value beyond the final one, so past kAudibleRadius the sizzle stays silent.
// The shape is loud up close and tapers off quickly, the way a real fire falls
// away as you back off it -- without a curve, FMOD's default inverse rolloff over
// a radius this large is nearly flat, which is why the sizzle would otherwise
// sound the same everywhere.
//
// x is distance in world units (metres), y is volume in [0, 1], z is unused.
// Not const: FMOD::Sound::set3DCustomRolloff takes a non-const pointer and, per
// its contract, does NOT copy the array -- it reads through this pointer for the
// life of the sound. So the points must outlive every voice, which file scope
// guarantees.
FMOD_VECTOR g_sizzle_curve_points[] = {
    {0.00f * kAudibleRadius, 1.00f, 0.0f}, {0.10f * kAudibleRadius, 0.70f, 0.0f},
    {0.30f * kAudibleRadius, 0.40f, 0.0f}, {0.60f * kAudibleRadius, 0.15f, 0.0f},
    {1.00f * kAudibleRadius, 0.00f, 0.0f},
};
constexpr int kSizzleCurvePointCount =
    static_cast<int>(sizeof(g_sizzle_curve_points) / sizeof(g_sizzle_curve_points[0]));

// The ear and the emitter carry no velocity: the listener moves but the game
// models no Doppler yet (that is what dt in Update is reserved for).
constexpr FMOD_VECTOR kNoVelocity{0.0f, 0.0f, 0.0f};

} // namespace

Audio::Audio() {
    // A machine with no audio device is not a crash: any failure here just leaves
    // silent_ set, and Update becomes a no-op. The game runs on without sound.
    if (FMOD::System_Create(&system_) != FMOD_OK) {
        silent_ = true;
        return;
    }

    // 32 virtual voices is far more than the single sound this game plays, but
    // costs nothing to leave as headroom. FMOD_INIT_NORMAL keeps FMOD's default
    // left-handed 3D coordinates, which already match the world (+X right, +Y up,
    // +Z forward) -- so unlike X3DAudio there is no handedness flag to flip and no
    // mirrored stereo image to correct.
    if (system_->init(32, FMOD_INIT_NORMAL, nullptr) != FMOD_OK) {
        system_->release();
        system_ = nullptr;
        silent_ = true;
        return;
    }

    if (FMOD::ChannelGroup* master = nullptr; system_->getMasterChannelGroup(&master) == FMOD_OK) {
        master->setVolume(kMasterVolume);
    }

    const std::filesystem::path wav = ExecutableDirectory() / "assets" / "audio" / "sizzle.wav";
    // FMOD takes filenames as UTF-8 on Windows; u8string() gives exactly that,
    // where path::string() would hand back the system ANSI code page.
    const std::u8string wav_utf8 = wav.u8string();

    // FMOD_3D positions the voice in the world; FMOD_3D_CUSTOMROLLOFF is what makes
    // set3DCustomRolloff below take effect (without it FMOD ignores the curve and
    // uses inverse rolloff). FMOD_LOOP_NORMAL because the grill is always lit.
    // FMOD_CREATESAMPLE decodes the whole 2 s clip into memory up front -- it is
    // tiny, and a memory-resident sample loops seamlessly with no streaming seam.
    const FMOD_MODE mode = FMOD_3D | FMOD_3D_CUSTOMROLLOFF | FMOD_LOOP_NORMAL | FMOD_CREATESAMPLE;
    if (system_->createSound(reinterpret_cast<const char*>(wav_utf8.c_str()), mode, nullptr,
                             &sizzle_) != FMOD_OK) {
        system_->release();
        system_ = nullptr;
        silent_ = true;
        return;
    }
    sizzle_->set3DCustomRolloff(g_sizzle_curve_points, kSizzleCurvePointCount);

    // Playback deliberately does not start here -- see started_ in the header and
    // the first-frame handling in Update.
}

Audio::~Audio() {
    if (system_) {
        // release closes the output and frees every sound created on the system,
        // so the loaded sizzle needs no separate release.
        system_->release();
        system_ = nullptr;
    }
}

void Audio::Update(FXMMATRIX camera_to_world, float dt) {
    if (silent_) {
        return;
    }

    XMFLOAT3 position;
    XMFLOAT3 up;
    XMFLOAT3 forward;
    XMStoreFloat3(&position, camera_to_world.r[3]);
    XMStoreFloat3(&up, camera_to_world.r[1]);
    XMStoreFloat3(&forward, camera_to_world.r[2]);

    const FMOD_VECTOR listener_pos{position.x, position.y, position.z};
    const FMOD_VECTOR listener_fwd{forward.x, forward.y, forward.z};
    const FMOD_VECTOR listener_up{up.x, up.y, up.z};
    system_->set3DListenerAttributes(0, &listener_pos, &kNoVelocity, &listener_fwd, &listener_up);

    // Begin the loop only now that the listener is posed. Start the voice paused,
    // place the (stationary) emitter, then unpause -- so the sizzle's first sample
    // already carries the correct distance attenuation and pan rather than a
    // full-volume blast from nowhere. The emitter never moves again, so this is
    // the only place it needs setting. Loops for as long as the grill is lit --
    // which is always, for now.
    if (!started_) {
        if (system_->playSound(sizzle_, nullptr, /*paused=*/true, &channel_) == FMOD_OK) {
            channel_->set3DAttributes(&kGrillEmitter, &kNoVelocity);
            channel_->setPaused(false);
            started_ = true;
        }
    }

    // FMOD mixes on its own thread; this hands the engine the frame in which to
    // service the mixer and act on a device change (which FMOD reroutes on its
    // own). A hard failure here will not fix itself, so the game goes quiet for
    // good rather than re-solving 3D audio for a device that is gone.
    if (system_->update() != FMOD_OK) {
        silent_ = true;
    }

    (void)dt; // FMOD keeps its own clock; dt is here for a future footstep or Doppler pass.
}
