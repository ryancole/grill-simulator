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
# No C++ modules in this project; skipping the scan removes a per-file clang-scan-deps pass.
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
include(Fonts)
include(Assets)

# Header-only TOML parser for the level files (see src/level.cpp), from vcpkg.
find_package(tomlplusplus CONFIG REQUIRED)

add_executable(grill WIN32
  src/main.cpp
  src/audio.cpp
  src/camera.cpp
  src/collision.cpp
  src/image.cpp
  src/input.cpp
  src/level.cpp
  src/model.cpp
  src/scene.cpp
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
target_link_libraries(grill PRIVATE Agility::SDK fastgltf::fastgltf fmod::core PhysX::PhysX tomlplusplus::tomlplusplus windowscodecs)

target_compile_definitions(grill PRIVATE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX)

target_compile_options(grill PRIVATE
  /W4
  /permissive-
  /utf-8
  # DirectX headers declare plenty of unused interface parameters.
  -Wno-unused-parameter
  $<$<CONFIG:Debug>:/Od>
  $<$<CONFIG:Release>:/O2>)

agility_sdk_stage_runtime(grill)
fmod_stage_runtime(grill)

add_hlsl_shader(grill SOURCE shaders/scene.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT scene.vs.cso)
add_hlsl_shader(grill SOURCE shaders/scene.hlsl ENTRY PSMain PROFILE ps_6_6 OUTPUT scene.ps.cso)
# The capture variant of the scene pixel shader: reflects the analytic sky rather
# than the probe, so it can fill the probe cubemap without reading it.
add_hlsl_shader(grill SOURCE shaders/scene.hlsl ENTRY PSMainCapture PROFILE ps_6_6 OUTPUT scene_capture.ps.cso)
add_hlsl_shader(grill SOURCE shaders/sky.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT sky.vs.cso)
add_hlsl_shader(grill SOURCE shaders/sky.hlsl ENTRY PSMain PROFILE ps_6_6 OUTPUT sky.ps.cso)
add_hlsl_shader(grill SOURCE shaders/text.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT text.vs.cso)
add_hlsl_shader(grill SOURCE shaders/text.hlsl ENTRY PSMain PROFILE ps_6_6 OUTPUT text.ps.cso)
add_hlsl_shader(grill SOURCE shaders/outline.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT outline.vs.cso)
add_hlsl_shader(grill SOURCE shaders/outline.hlsl ENTRY PSMain PROFILE ps_6_6 OUTPUT outline.ps.cso)
# Depth only, so the shadow pass has a vertex shader and no pixel shader.
add_hlsl_shader(grill SOURCE shaders/shadow.hlsl ENTRY VSMain PROFILE vs_6_6 OUTPUT shadow.vs.cso)

stage_assets(grill DIRECTORY assets/models
  FILES grill-basic.glb tree.glb bench-basic.glb crate.glb cooler-basic.glb tongs-metal.glb burger-raw.glb steak-cooked.glb)

stage_assets(grill DIRECTORY assets/audio
  FILES sizzle.wav 232135__yottasounds__splat-005.wav 446128__justinvoke__metal-clank-4.wav)

# The levels are text, parsed at runtime (see src/level.cpp), so editing one and
# re-staging needs no recompile -- only the copy next to the executable.
stage_assets(grill DIRECTORY assets/levels
  FILES backyard.level rooftop.level)

# The HUD font atlas is baked from the committed .ttf at build time; only the
# .png/.csv it produces are read at runtime, and those live in the build tree.
bake_font_atlas(grill FONT assets/fonts/Inter-Regular.ttf IMAGE hud.png CSV hud.csv)
