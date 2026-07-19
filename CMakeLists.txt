cmake_minimum_required(VERSION 3.28)

project(grill_simulator
  VERSION 0.1.0
  DESCRIPTION "A 3D grill game on raw Direct3D 12"
  LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
  message(FATAL_ERROR "grill-simulator is Windows-only: it targets raw Direct3D 12.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++ modules in this project; skipping the scan removes a per-file dependency-scan pass.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# One predictable place per configuration for the .exe, the staged Agility
# runtime and the compiled shaders. The shader rules need this path spelled out
# at configure time, so it cannot be left to the generator's default.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AgilitySDK)
include(Shaders)
include(FastGltf)
include(Fmod)
include(PhysX)
include(Flow)
include(Fonts)
include(Assets)

# Header-only TOML parser for the level files (see src/level.cpp), from vcpkg.
find_package(tomlplusplus CONFIG REQUIRED)

add_executable(grill WIN32
  src/main.cpp
  src/actions.cpp
  src/audio.cpp
  src/camera.cpp
  src/catalog.cpp
  src/collision.cpp
  src/cook_information.cpp
  src/environment.cpp
  src/flame.cpp
  src/flow_volume.cpp
  src/fluid.cpp
  src/heat_source.cpp
  src/ignitable_requirements.cpp
  src/image.cpp
  src/input.cpp
  src/level.cpp
  src/menu.cpp
  src/model.cpp
  src/objectives.cpp
  src/scene.cpp
  src/serve_zone.cpp
  src/physics.cpp
  src/props.cpp
  src/furniture.cpp
  src/rigid_body.cpp
  src/viewmodel.cpp
  src/font.cpp
  src/world.cpp
  src/renderer.cpp
  src/agility_exports.cpp)

target_include_directories(grill PRIVATE src)
# windowscodecs is WIC, which decodes the PNG and JPEG images glTF embeds. It
# ships with Windows, so it costs the project no third-party dependency.
target_link_libraries(grill PRIVATE Agility::SDK fastgltf::fastgltf fmod::core Flow::Flow PhysX::PhysX tomlplusplus::tomlplusplus windowscodecs)

target_compile_definitions(grill PRIVATE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX)

target_compile_options(grill PRIVATE
  /W4
  /permissive-
  /utf-8
  # DirectX headers declare plenty of unused interface parameters (C4100).
  /wd4100
  $<$<CONFIG:Debug>:/Od>
  $<$<CONFIG:Release>:/O2>)

agility_sdk_stage_runtime(grill)
fmod_stage_runtime(grill)
physx_stage_gpu_runtime(grill)
flow_stage_runtime(grill)

add_hlsl_shader(grill SOURCE shaders/scene.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT scene.vs.cso)
add_hlsl_shader(grill SOURCE shaders/scene.hlsl ENTRY PSMain PROFILE ps_6_6 OUTPUT scene.ps.cso)
# The capture variant of the scene pixel shader: reflects the analytic sky rather
# than the probe, so it can fill the probe cubemap without reading it.
add_hlsl_shader(grill SOURCE shaders/scene.hlsl ENTRY PSMainCapture PROFILE ps_6_6 OUTPUT scene_capture.ps.cso)
add_hlsl_shader(grill SOURCE shaders/sky.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT sky.vs.cso)
add_hlsl_shader(grill SOURCE shaders/sky.hlsl ENTRY PSMain PROFILE ps_6_6 OUTPUT sky.ps.cso)
# The capture variant of the sky pixel shader: encodes to sRGB for the 8-bit probe
# cube, where the on-screen one leaves its radiance linear for the HDR buffer.
add_hlsl_shader(grill SOURCE shaders/sky.hlsl ENTRY PSMainCapture PROFILE ps_6_6 OUTPUT sky_capture.ps.cso)
# The resolve pass: tonemaps the linear HDR scene buffer and encodes it to sRGB.
add_hlsl_shader(grill SOURCE shaders/tonemap.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT tonemap.vs.cso)
add_hlsl_shader(grill SOURCE shaders/tonemap.hlsl ENTRY PSMain PROFILE ps_6_6 OUTPUT tonemap.ps.cso)
# The volumetric sun-shaft pass: marches the shadow map and adds scattered light.
add_hlsl_shader(grill SOURCE shaders/lightshafts.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT lightshafts.vs.cso)
add_hlsl_shader(grill SOURCE shaders/lightshafts.hlsl ENTRY PSMain PROFILE ps_6_6 OUTPUT lightshafts.ps.cso)
# The volumetric cloud pass: marches a density slab overhead and composites it over the sky.
add_hlsl_shader(grill SOURCE shaders/clouds.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT clouds.vs.cso)
add_hlsl_shader(grill SOURCE shaders/clouds.hlsl ENTRY PSMain PROFILE ps_6_6 OUTPUT clouds.ps.cso)
# The lighter-fluid spray: a screen-space fluid pipeline. Pass 1 (fluid) billboards each
# droplet and writes nearest surface depth + accumulated thickness; pass 2 (fluid_blur)
# bilaterally smooths the depth so beads merge into a surface; pass 3 (fluid_composite)
# reconstructs the surface and composites it as lit, tinted, translucent liquid.
add_hlsl_shader(grill SOURCE shaders/fluid.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT fluid.vs.cso)
add_hlsl_shader(grill SOURCE shaders/fluid.hlsl ENTRY PSMain PROFILE ps_6_6 OUTPUT fluid.ps.cso)
add_hlsl_shader(grill SOURCE shaders/fluid_blur.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT fluid_blur.vs.cso)
add_hlsl_shader(grill SOURCE shaders/fluid_blur.hlsl ENTRY PSMain PROFILE ps_6_6 OUTPUT fluid_blur.ps.cso)
add_hlsl_shader(grill SOURCE shaders/fluid_composite.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT fluid_composite.vs.cso)
add_hlsl_shader(grill SOURCE shaders/fluid_composite.hlsl ENTRY PSMain PROFILE ps_6_6 OUTPUT fluid_composite.ps.cso)
# The bloom pyramid: one vertex shader, a downsample and an upsample pixel shader.
add_hlsl_shader(grill SOURCE shaders/bloom.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT bloom.vs.cso)
add_hlsl_shader(grill SOURCE shaders/bloom.hlsl ENTRY PSDownsample PROFILE ps_6_6 OUTPUT bloom_down.ps.cso)
add_hlsl_shader(grill SOURCE shaders/bloom.hlsl ENTRY PSUpsample PROFILE ps_6_6 OUTPUT bloom_up.ps.cso)
add_hlsl_shader(grill SOURCE shaders/text.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT text.vs.cso)
add_hlsl_shader(grill SOURCE shaders/text.hlsl ENTRY PSMain PROFILE ps_6_6 OUTPUT text.ps.cso)
add_hlsl_shader(grill SOURCE shaders/outline.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT outline.vs.cso)
add_hlsl_shader(grill SOURCE shaders/outline.hlsl ENTRY PSMain PROFILE ps_6_6 OUTPUT outline.ps.cso)
# Depth only, so the shadow pass has a vertex shader and no pixel shader.
add_hlsl_shader(grill SOURCE shaders/shadow.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT shadow.vs.cso)
# Volumetric grass: an amplification shader culls cells and picks a level of detail, a
# mesh shader grows the surviving blades on the GPU (no vertex buffer), a pixel shader
# shades them. Only loaded where the device reports mesh-shader tier 1.
add_hlsl_shader(grill SOURCE shaders/grass.hlsl ENTRY ASMain PROFILE as_6_6 OUTPUT grass.as.cso)
add_hlsl_shader(grill SOURCE shaders/grass.hlsl ENTRY MSMain PROFILE ms_6_6 OUTPUT grass.ms.cso)
add_hlsl_shader(grill SOURCE shaders/grass.hlsl ENTRY PSMain PROFILE ps_6_6 OUTPUT grass.ps.cso)

stage_assets(grill DIRECTORY assets/models
  FILES charcoal-grill-base.glb charcoal-grill-lid.glb tree.glb bench-basic.glb crate.glb cooler-basic.glb tray.glb tongs-metal.glb burger-raw.glb steak-cooked.glb chicken-raw.glb chicken-cooked.glb firepit.glb tent.glb log.glb lighter-fluid.glb lighter.glb)

stage_assets(grill DIRECTORY assets/audio
  FILES sizzle.wav 232135__yottasounds__splat-005.wav 446128__justinvoke__metal-clank-4.wav
        820059__lukemeney__round-baking-tray-new-1.wav 820060__lukemeney__round-baking-tray-new-2.wav
        820057__lukemeney__big-pot-top-2.wav 820058__lukemeney__big-pot-top-3.wav)

# The levels are text, parsed at runtime (see src/level.cpp), so editing one and
# re-staging needs no recompile -- only the copy next to the executable.
stage_assets(grill DIRECTORY assets/levels
  FILES backyard.toml campsite.toml)

# The control bindings and the object catalog, likewise text parsed at runtime (see
# src/actions.cpp, src/catalog.cpp): editing one is a text edit and a re-stage, no
# recompile.
stage_assets(grill DIRECTORY assets
  FILES controls.toml catalog.toml)

# The HUD font atlas is baked from the committed .ttf at build time; only the
# .png/.csv it produces are read at runtime, and those live in the build tree.
bake_font_atlas(grill FONT assets/fonts/Inter-Regular.ttf IMAGE hud.png CSV hud.csv)
# A second atlas, monospace, baked the same way -- the debug overlay draws from it
# so its columns line up; the prompt and menu keep the Inter atlas above.
bake_font_atlas(grill FONT assets/fonts/JetBrainsMono-Regular.ttf IMAGE mono.png CSV mono.csv)
