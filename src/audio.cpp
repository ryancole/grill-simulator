#include "audio.hpp"

#include "dx_common.hpp"

using namespace DirectX;

namespace {

// The grill instance stands at (0, 0, 5) in scene.cpp; this lifts the emitter to
// roughly grate height, so the sizzle comes off the cooking surface rather than
// the floor. There is only the one sound, so it is kept in step with the scene by
// hand rather than plumbed through from it.
constexpr XMFLOAT3 kGrillEmitter{0.0f, 0.9f, 5.0f};

// The one global volume knob, applied to the engine's master voice so it scales
// every sound at once. 1.0 is unity (the mix as authored); below that turns the
// whole game down, above that amplifies it. This is the dial to turn for overall
// loudness -- the per-emitter curve below only shapes falloff with distance.
constexpr float kMasterVolume = 0.5f;

// How far, in metres, the sizzle carries. X3DAudio normalises the emitter-to-ear
// distance against this before reading the curve below, so it is both the scale
// of the falloff and a hard cutoff: past this the curve's last point (silence) is
// held. The player spawns ~12 m from the grill, so at 11 m they hear nothing and
// have to walk in a step or two before it fades up.
constexpr float kAudibleRadius = 11.0f;

// The distance-to-volume curve, from the grate (normalised distance 0, full gain)
// out to the radius (normalised 1, silent). Without this X3DAudio uses a default
// rolloff that, stretched over a radius this large, is nearly flat -- which is why
// the sizzle used to sound the same everywhere. The shape here is loud up close
// and tapers off quickly, the way a real fire falls away as you back off it.
//
// Not const: X3DAUDIO_DISTANCE_CURVE points at these with non-const pointers, and
// X3DAudio only reads them. They must outlive every Apply3D call, so they are
// file-scope rather than locals in the constructor.
X3DAUDIO_DISTANCE_CURVE_POINT g_sizzle_curve_points[] = {
    {0.00f, 1.00f}, {0.10f, 0.70f}, {0.30f, 0.40f}, {0.60f, 0.15f}, {1.00f, 0.00f},
};
X3DAUDIO_DISTANCE_CURVE g_sizzle_curve{g_sizzle_curve_points,
                                       static_cast<UINT32>(ARRAYSIZE(g_sizzle_curve_points))};

} // namespace

Audio::Audio() {
    AUDIO_ENGINE_FLAGS flags = AudioEngine_Default;
#ifndef NDEBUG
    // Routes XAudio2's own diagnostics through the debug output; the debug layer
    // is only present when the runtime is installed, so it is a Debug-only ask.
    flags = flags | AudioEngine_Debug;
#endif
    // A machine with no audio device is not an error: AudioEngine constructs in a
    // silent state, and Update below simply keeps reporting there is nothing to do.
    engine_ = std::make_unique<AudioEngine>(flags);
    engine_->SetMasterVolume(kMasterVolume);

    const std::filesystem::path wav = ExecutableDirectory() / "assets" / "audio" / "sizzle.wav";
    sizzle_ = std::make_unique<SoundEffect>(engine_.get(), wav.wstring().c_str());

    // Use3D is what lets Apply3D reposition the voice; without it the instance is
    // a flat stereo sound the listener could never walk around.
    sizzle_instance_ = sizzle_->CreateInstance(SoundEffectInstance_Use3D);

    // A single-point source, attenuated by the curve above out to kAudibleRadius.
    emitter_.pVolumeCurve = &g_sizzle_curve;
    emitter_.CurveDistanceScaler = kAudibleRadius;
    emitter_.SetPosition(kGrillEmitter);

    sizzle_instance_->Play(true); // Loop for as long as the grill is lit -- which is always, for now.
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

    listener_.SetPosition(position);
    listener_.SetOrientation(forward, up);

    // The world is left-handed (+X right, +Y up, +Z forward), so X3DAudio must be
    // told not to assume right-handed coordinates or the stereo image is mirrored.
    sizzle_instance_->Apply3D(listener_, emitter_, /*rhcoords=*/false);

    // XAudio2 mixes on its own thread; this hands the engine the frame in which to
    // recycle finished voices and act on a device change. A false return with no
    // critical error just means there is no device -- harmless, and worth
    // retrying every frame in case one appears. A critical error will not fix
    // itself, so the game goes quiet for good.
    if (!engine_->Update()) {
        if (engine_->IsCriticalError()) {
            silent_ = true;
        }
    }

    (void)dt; // The engine keeps its own clock; dt is here for a future footstep or Doppler pass.
}
