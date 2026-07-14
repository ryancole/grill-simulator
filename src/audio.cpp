#include "audio.hpp"

#include "dx_common.hpp"
#include "rigid_body.hpp"

#include <fmod.hpp>
#include <fmod_errors.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

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

// An impact's distance falloff, driven by FMOD's default inverse rolloff: full
// gain within the near distance, then thinning as the near/distance ratio. The
// meat and tongs usually land right by the player, but a thrown one can sound
// across the yard, so it stays just audible out to the far distance and no
// further. Shared by both one-shots for now.
constexpr float kImpactNearDistance = 1.5f;  // metres of full-gain radius.
constexpr float kImpactFarDistance = 25.0f;  // past here the gain stops falling.

// Maps a contact impulse to a volume. At or above the reference impulse the clip
// plays at full gain; a gentle set-down floors at the minimum so it is still heard
// rather than silent. Tuned by ear against the props' masses and drop heights --
// the tongs are lighter than the meat, so if their clank comes out too quiet this
// reference (or the min-impulse gate in physics.cpp) is the dial to drop.
constexpr float kImpactFullVolumeImpulse = 4.0f;
constexpr float kImpactMinVolume = 0.25f;

// Each impact is pitched by a random factor in this band, so a run of them (a
// patty bouncing, or several dropped in a row) does not machine-gun the sample.
constexpr float kImpactPitchLow = 0.9f;
constexpr float kImpactPitchHigh = 1.12f;

// One generator for the pitch jitter, seeded once. Playback is all on the game
// thread (PlayImpact is called from the loop), so no synchronisation is needed.
std::mt19937 g_pitch_rng{std::random_device{}()};

// Loads one impact one-shot from the audio folder: FMOD_3D so it pans and
// attenuates from the landing spot, FMOD_LOOP_OFF because it fires once per hit,
// FMOD_CREATESAMPLE so the tiny clip is decoded up front and a fresh voice starts
// with no streaming seam. Its falloff is FMOD's default inverse rolloff shaped by
// the near/far distances, so no custom curve and no FMOD_3D_CUSTOMROLLOFF. Returns
// null on failure -- a missing impact clip is not fatal, the caller runs on.
FMOD::Sound* LoadImpactSound(FMOD::System* system, const char* filename) {
    const std::filesystem::path path = ExecutableDirectory() / "assets" / "audio" / filename;
    // FMOD takes UTF-8 filenames on Windows; u8string() gives exactly that.
    const std::u8string utf8 = path.u8string();
    const FMOD_MODE mode = FMOD_3D | FMOD_LOOP_OFF | FMOD_CREATESAMPLE;
    FMOD::Sound* sound = nullptr;
    if (system->createSound(reinterpret_cast<const char*>(utf8.c_str()), mode, nullptr, &sound) !=
        FMOD_OK) {
        return nullptr;
    }
    sound->set3DMinMaxDistance(kImpactNearDistance, kImpactFarDistance);
    return sound;
}

// One of a sound's interchangeable takes, chosen at random -- or null if none of
// them loaded. Picking per hit keeps a run of impacts from repeating one sample.
FMOD::Sound* PickVariant(const std::vector<FMOD::Sound*>& clips) {
    if (clips.empty()) {
        return nullptr;
    }
    std::uniform_int_distribution<std::size_t> pick(0, clips.size() - 1);
    return clips[pick(g_pitch_rng)];
}

// Loads each named clip and keeps the ones that came up, so a missing take just
// narrows the variety rather than failing. Shared by the grill's base and lid.
std::vector<FMOD::Sound*> LoadVariants(FMOD::System* system,
                                       std::initializer_list<const char*> files) {
    std::vector<FMOD::Sound*> clips;
    for (const char* file : files) {
        if (FMOD::Sound* sound = LoadImpactSound(system, file)) {
            clips.push_back(sound);
        }
    }
    return clips;
}

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

    // The impact one-shots: a wet splat for the meat, a metal clank for the tongs,
    // and the grill's own knocks -- the base like the baking tray it is shaped like,
    // the lid like a pot top -- each in two takes PlayImpact chooses between. A
    // missing clip is not fatal -- the sizzle already loaded, so the game runs on and
    // PlayImpact simply skips whichever sound failed (it null-checks the clip).
    splat_ = LoadImpactSound(system_, "232135__yottasounds__splat-005.wav");
    clank_ = LoadImpactSound(system_, "446128__justinvoke__metal-clank-4.wav");
    grill_base_ = LoadVariants(system_, {"820059__lukemeney__round-baking-tray-new-1.wav",
                                         "820060__lukemeney__round-baking-tray-new-2.wav"});
    grill_lid_ = LoadVariants(system_, {"820057__lukemeney__big-pot-top-2.wav",
                                        "820058__lukemeney__big-pot-top-3.wav"});

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

void Audio::PlayImpact(const XMFLOAT3& position, float strength, ImpactSound sound) {
    // Before the first Update the listener is unposed, so an impact then would
    // sound from nowhere -- skip it (a hit on frame zero is not a real case). Also
    // nothing to play into if the engine never came up.
    if (silent_ || !started_) {
        return;
    }

    // Pick the clip this impact calls for. A sound whose clip failed to load (null)
    // just plays nothing, same as None.
    FMOD::Sound* clip = nullptr;
    switch (sound) {
    case ImpactSound::Meat:
        clip = splat_;
        break;
    case ImpactSound::Metal:
        clip = clank_;
        break;
    case ImpactSound::GrillBase:
        clip = PickVariant(grill_base_);
        break;
    case ImpactSound::GrillLid:
        clip = PickVariant(grill_lid_);
        break;
    case ImpactSound::None:
        break;
    }
    if (clip == nullptr) {
        return;
    }

    // Louder the harder it hit, down to a floor so a soft set-down is still heard.
    const float volume = std::clamp(strength / kImpactFullVolumeImpulse, kImpactMinVolume, 1.0f);
    std::uniform_real_distribution<float> pitch(kImpactPitchLow, kImpactPitchHigh);

    // Start paused so the emitter is placed and the voice shaped before its first
    // sample is heard, then unpause -- the same one-shot pattern as the sizzle's
    // first frame. A null channel (all voices busy) just means this impact is
    // dropped, which is fine under a pile-up.
    FMOD::Channel* channel = nullptr;
    if (system_->playSound(clip, nullptr, /*paused=*/true, &channel) != FMOD_OK ||
        channel == nullptr) {
        return;
    }
    const FMOD_VECTOR emitter{position.x, position.y, position.z};
    channel->set3DAttributes(&emitter, &kNoVelocity);
    channel->setVolume(volume);
    channel->setPitch(pitch(g_pitch_rng));
    channel->setPaused(false);
}
