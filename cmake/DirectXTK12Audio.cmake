# DirectX Tool Kit for Audio, pulled straight from GitHub.
#
# https://github.com/microsoft/DirectXTK12/wiki/Audio
#
# DirectXTK12 is Microsoft's helper library over the raw Windows APIs -- for us,
# the audio equivalent of the d3dx12 helpers over raw D3D12. Its Audio module is
# a thin, self-contained wrapper over XAudio2 2.9 and X3DAudio: WAV loading, a
# pool of source voices, looping, and 3D panning (SoundEffectInstance::Apply3D).
#
# We deliberately do NOT run DirectXTK12's own CMakeLists. That builds the entire
# graphics toolkit and runs DXC over ~100 HLSL shaders at configure/build time --
# none of which the audio module touches. Instead we populate the source tree
# (the fastgltf trick: SOURCE_SUBDIR names a directory with no CMakeLists.txt, so
# MakeAvailable downloads without add_subdirectory) and compile just the nine
# Audio/*.cpp files into a small static library of our own. No shaders, no DXC,
# no graphics .cpp -- exactly the "just the audio module" this project wants.
#
# What the audio sources need from the rest of the toolkit is only headers:
#   Inc/     Audio.h, DirectXHelpers.h        (public)
#   Src/     pch.h, PlatformHelpers.h, d3dx12.h (shared, vendored)
#   Audio/   SoundCommon.h, WaveBankReader.h, WAVFileReader.h (private)
# The shared pch.h pulls in <d3d12.h>/d3dx12.h, but as headers only -- the audio
# code never calls into D3D. runtimeobject.lib is auto-linked by a #pragma inside
# the sources; only xaudio2.lib has to be named, and it ships in the Windows SDK.

include(FetchContent)

set(DIRECTXTK12_VERSION "may2026" CACHE STRING "DirectXTK12 release tag")

FetchContent_Declare(directxtk12
  URL "https://github.com/microsoft/DirectXTK12/archive/refs/tags/${DIRECTXTK12_VERSION}.tar.gz"
  URL_HASH SHA512=421b154852db4f52de25c658ae53e70a286bebd0ff91291d146cc22045ae208e5b6051e6acb38b9de62b3e7db466bc3de7a24c4b323fe7b2907fd21a51dc43cf
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR does-not-exist)
FetchContent_MakeAvailable(directxtk12)

set(_audio_dir "${directxtk12_SOURCE_DIR}/Audio")

add_library(dxtk12_audio STATIC
  "${_audio_dir}/AudioEngine.cpp"
  "${_audio_dir}/DynamicSoundEffectInstance.cpp"
  "${_audio_dir}/SoundCommon.cpp"
  "${_audio_dir}/SoundEffect.cpp"
  "${_audio_dir}/SoundEffectInstance.cpp"
  "${_audio_dir}/SoundStreamInstance.cpp"
  "${_audio_dir}/WaveBank.cpp"
  "${_audio_dir}/WaveBankReader.cpp"
  "${_audio_dir}/WAVFileReader.cpp")
add_library(DirectXTK12::Audio ALIAS dxtk12_audio)

# Src and Audio are the toolkit's own private include roots; Inc is the public
# one, and the only one a consumer of Audio.h needs. Marking Inc SYSTEM keeps the
# game's /W4 from firing on the toolkit's headers, exactly as fastgltf does.
target_include_directories(dxtk12_audio
  PRIVATE
    "${directxtk12_SOURCE_DIR}/Src"
    "${_audio_dir}"
  SYSTEM PUBLIC
    "${directxtk12_SOURCE_DIR}/Inc")

# 0x0A00 is what selects the XAudio 2.9 path inside Audio.h (it defines
# USING_XAUDIO2_9 from _WIN32_WINNT). PUBLIC so the game and this library agree on
# it -- Audio.h picks its struct layouts from the same switch on both sides.
target_compile_definitions(dxtk12_audio PUBLIC _WIN32_WINNT=0x0A00)

# xaudio2.lib is the only import library the sources need named by hand;
# runtimeobject.lib comes in through a #pragma comment(lib) in the toolkit.
target_link_libraries(dxtk12_audio PUBLIC xaudio2)

# Third-party code. It builds clean under clang-cl, but it is not ours to hold to
# our warning bar, and its own pch.h already silences what it expects to.
target_compile_options(dxtk12_audio PRIVATE -w)
