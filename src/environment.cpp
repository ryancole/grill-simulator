#include "environment.hpp"

#include <array>
#include <cmath>

using namespace DirectX;

namespace {

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

XMFLOAT3 Lerp3(const XMFLOAT3& a, const XMFLOAT3& b, float t) {
    return {Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t)};
}

XMFLOAT2 Lerp2(const XMFLOAT2& a, const XMFLOAT2& b, float t) {
    return {Lerp(a.x, b.x, t), Lerp(a.y, b.y, t)};
}

// Blend two whole skies, field by field. Everything is a colour or a scalar that
// reads sensibly under a straight linear blend, so the whole struct interpolates --
// the fields a keyframe leaves at the default simply blend default-to-default.
Environment LerpEnvironment(const Environment& a, const Environment& b, float t) {
    // Start from the default so any field not blended below (the volumetric cloud
    // slab -- cloud_bottom/top/density/detail -- which every keyframe leaves at its
    // default) keeps a sane value rather than reading uninitialised garbage.
    Environment e = kDefaultEnvironment;
    e.sky.zenith = Lerp3(a.sky.zenith, b.sky.zenith, t);
    e.sky.cloud_scale = Lerp(a.sky.cloud_scale, b.sky.cloud_scale, t);
    e.sky.horizon = Lerp3(a.sky.horizon, b.sky.horizon, t);
    e.sky.cloud_coverage = Lerp(a.sky.cloud_coverage, b.sky.cloud_coverage, t);
    e.sky.ground = Lerp3(a.sky.ground, b.sky.ground, t);
    e.sky.cloud_softness = Lerp(a.sky.cloud_softness, b.sky.cloud_softness, t);
    e.sky.cloud_color = Lerp3(a.sky.cloud_color, b.sky.cloud_color, t);
    e.sky.cloud_opacity = Lerp(a.sky.cloud_opacity, b.sky.cloud_opacity, t);
    e.sky.cloud_wind = Lerp2(a.sky.cloud_wind, b.sky.cloud_wind, t);
    e.sky.pad0 = 0.0f;
    e.sky.pad1 = 0.0f;
    e.sun_color = Lerp3(a.sun_color, b.sun_color, t);
    e.sun_intensity = Lerp(a.sun_intensity, b.sun_intensity, t);
    e.sky_ambient = Lerp3(a.sky_ambient, b.sky_ambient, t);
    e.ambient_strength = Lerp(a.ambient_strength, b.ambient_strength, t);
    e.fill_strength = Lerp(a.fill_strength, b.fill_strength, t);
    e.fog_start = Lerp(a.fog_start, b.fog_start, t);
    e.fog_end = Lerp(a.fog_end, b.fog_end, t);
    e.shaft_color = Lerp3(a.shaft_color, b.shaft_color, t);
    e.shaft_intensity = Lerp(a.shaft_intensity, b.shaft_intensity, t);
    e.shaft_g = Lerp(a.shaft_g, b.shaft_g, t);
    e.exposure = Lerp(a.exposure, b.exposure, t);
    e.bloom_intensity = Lerp(a.bloom_intensity, b.bloom_intensity, t);
    e.bloom_threshold = Lerp(a.bloom_threshold, b.bloom_threshold, t);
    e.bloom_knee = Lerp(a.bloom_knee, b.bloom_knee, t);
    return e;
}

// One anchor of the day cycle: a clock hour, the sun's direction then, and the sky
// that goes with it. Between two anchors everything is linearly blended, so only the
// fields that read differently by time of day are set away from the default here --
// the structural cloud/fog/shaft-shape fields stay at their defaults and ride
// through untouched.
struct SkyKeyframe {
    float hour;
    XMFLOAT3 sun;
    Environment env;
};

// Build a keyframe environment from the default, overriding only the fields that a
// time of day actually changes. Keeps each anchor below to the handful of values
// that matter rather than a full struct literal.
Environment Mood(XMFLOAT3 zenith, XMFLOAT3 horizon, XMFLOAT3 ground, XMFLOAT3 sun_color,
                 float sun_intensity, XMFLOAT3 sky_ambient, float ambient_strength,
                 XMFLOAT3 cloud_color, float cloud_coverage, XMFLOAT3 shaft_color,
                 float shaft_intensity) {
    Environment e = kDefaultEnvironment;
    e.sky.zenith = zenith;
    e.sky.horizon = horizon;
    e.sky.ground = ground;
    e.sky.cloud_color = cloud_color;
    e.sky.cloud_coverage = cloud_coverage;
    e.sun_color = sun_color;
    e.sun_intensity = sun_intensity;
    e.sky_ambient = sky_ambient;
    e.ambient_strength = ambient_strength;
    e.shaft_color = shaft_color;
    e.shaft_intensity = shaft_intensity;
    return e;
}

// The day, as four moods on the clock plus the midnight wrap: deep night, a warm low
// sunrise in the east, the default midday blue overhead at noon, a deep sunset in the
// west, and back to night. The sun's y is its height; x runs east(+) to west(-); a
// slight south (-z) keeps shadows from lying dead flat. Hours between anchors blend
// both the direction and the sky.
const std::array<SkyKeyframe, 5> kDayCycle = {{
    {0.0f,
     {0.20f, 0.80f, -0.30f}, // a dim high "moon" so night light still falls from above
     Mood(/*zenith*/ {0.03f, 0.04f, 0.09f}, /*horizon*/ {0.06f, 0.08f, 0.15f},
          /*ground*/ {0.02f, 0.02f, 0.03f}, /*sun*/ {0.55f, 0.62f, 0.80f}, /*sun_int*/ 0.35f,
          /*ambient*/ {0.07f, 0.09f, 0.16f}, /*amb_str*/ 0.55f, /*cloud*/ {0.30f, 0.34f, 0.46f},
          /*coverage*/ 0.42f, /*shaft*/ {0.45f, 0.55f, 0.75f}, /*shaft_int*/ 0.30f)},
    {6.0f,
     {0.90f, 0.15f, -0.20f}, // low in the east
     Mood(/*zenith*/ {0.34f, 0.42f, 0.62f}, /*horizon*/ {0.98f, 0.64f, 0.46f},
          /*ground*/ {0.22f, 0.18f, 0.16f}, /*sun*/ {1.0f, 0.74f, 0.50f}, /*sun_int*/ 2.2f,
          /*ambient*/ {0.64f, 0.56f, 0.56f}, /*amb_str*/ 0.62f, /*cloud*/ {1.0f, 0.86f, 0.80f},
          /*coverage*/ 0.50f, /*shaft*/ {1.0f, 0.72f, 0.52f}, /*shaft_int*/ 1.2f)},
    {12.0f,
     {0.10f, 0.95f, -0.30f}, // near overhead, a touch south
     kDefaultEnvironment},   // noon is exactly the built-in default look
    {18.0f,
     {-0.90f, 0.12f, 0.15f}, // low in the west
     Mood(/*zenith*/ {0.20f, 0.24f, 0.46f}, /*horizon*/ {0.99f, 0.48f, 0.28f},
          /*ground*/ {0.22f, 0.16f, 0.12f}, /*sun*/ {1.0f, 0.58f, 0.34f}, /*sun_int*/ 1.9f,
          /*ambient*/ {0.56f, 0.44f, 0.46f}, /*amb_str*/ 0.56f, /*cloud*/ {1.0f, 0.78f, 0.70f},
          /*coverage*/ 0.55f, /*shaft*/ {1.0f, 0.56f, 0.36f}, /*shaft_int*/ 1.3f)},
    {24.0f,
     {0.20f, 0.80f, -0.30f}, // wraps to the 0:00 midnight anchor
     Mood(/*zenith*/ {0.03f, 0.04f, 0.09f}, /*horizon*/ {0.06f, 0.08f, 0.15f},
          /*ground*/ {0.02f, 0.02f, 0.03f}, /*sun*/ {0.55f, 0.62f, 0.80f}, /*sun_int*/ 0.35f,
          /*ambient*/ {0.07f, 0.09f, 0.16f}, /*amb_str*/ 0.55f, /*cloud*/ {0.30f, 0.34f, 0.46f},
          /*coverage*/ 0.42f, /*shaft*/ {0.45f, 0.55f, 0.75f}, /*shaft_int*/ 0.30f)},
}};

} // namespace

Environment EnvironmentAtHour(float hours, XMFLOAT3& sun_direction) {
    // Wrap into [0,24) so 25:00 is 1am and -1:00 is 23:00.
    hours = std::fmod(hours, 24.0f);
    if (hours < 0.0f) {
        hours += 24.0f;
    }

    // Find the anchor pair bracketing the hour. The table spans 0..24 inclusive, so
    // some pair always contains it.
    for (std::size_t i = 0; i + 1 < kDayCycle.size(); ++i) {
        const SkyKeyframe& a = kDayCycle[i];
        const SkyKeyframe& b = kDayCycle[i + 1];
        if (hours <= b.hour) {
            const float t = (hours - a.hour) / (b.hour - a.hour);
            sun_direction = Lerp3(a.sun, b.sun, t);
            return LerpEnvironment(a.env, b.env, t);
        }
    }

    // Unreachable given the wrap above, but keep the sun and sky defined regardless.
    sun_direction = kDayCycle.back().sun;
    return kDayCycle.back().env;
}
