# NVIDIA PhysX 5, via the vcpkg "physx" port (Omniverse PhysX SDK 5.5.0).
#
# https://github.com/microsoft/vcpkg/tree/master/ports/physx  (port)
# https://github.com/NVIDIA-Omniverse/PhysX                    (BSD-3-Clause)
#
# PhysX has no clean FetchContent/add_subdirectory story of its own -- its build
# is driven by an in-repo generate_projects + packman bootstrap that fights this
# project's CMake presets / Ninja / VS2026 toolchain. The vcpkg port wraps all of that
# (packman, presets, CRT selection) and builds the SDK once from source, then
# hands back a normal CMake package. vcpkg is vendored as a pinned submodule under
# external/vcpkg and wired in through the toolchain file in CMakePresets.json, so
# a configure resolves and (on first run) builds PhysX with no extra steps.
#
# The chosen triplet is x64-windows-static-md: PhysX links *statically* with the
# *dynamic* CRT (/MD, /MDd), matching this project's CMAKE_MSVC_RUNTIME_LIBRARY.
#
# The port is built by vcpkg with MSVC, same as the game itself.

find_package(unofficial-omniverse-physx-sdk CONFIG REQUIRED)

# The rest of the build links one PhysX::PhysX target and stays agnostic about how
# PhysX is sourced. The port's target carries the include dirs and the full set of
# static libraries.
add_library(PhysX::PhysX INTERFACE IMPORTED GLOBAL)
target_link_libraries(PhysX::PhysX INTERFACE unofficial::omniverse-physx-sdk::sdk)

# The GPU pipeline is late-bound: nothing links these two libraries. PhysX's
# static core LoadLibrary()s PhysXGpu_64.dll when PxCreateCudaContextManager is
# called (and that pulls in PhysXDevice64.dll), so the pair just has to sit next
# to the executable.
#
# PhysXGpu_64.dll must match the CONFIGURATION of the SDK calling it. It is not
# the C boundary it looks like: the SDK hands the GPU library its internal
# structures, whose layout changes with PX_CHECKED/PX_DEBUG, so a debug SDK
# against the release GPU library access-violates a few frames into simulation
# (inside the DLL, ~0x91c57) rather than failing at load. NVIDIA publishes only
# checked/profile/release GPU builds -- there is no debug one -- and their own
# build pairs a *debug* SDK with the *checked* DLL:
#
#   physx/source/compiler/cmake/windows/CMakeLists.txt
#     FILE(COPY .../checked/PhysXGpu_64.dll DESTINATION ${PX_EXE_OUTPUT_DIRECTORY_DEBUG})
#
# The vcpkg port installs only the release DLL ("only release binaries should go
# in tools/"), so the checked one is fetched here from the same NVIDIA archive the
# port itself downloads, pinned by hash -- the Agility SDK / DXC pattern. Fetching
# rather than reading vcpkg's buildtrees is deliberate: a binary-cache hit skips
# the port's build entirely and leaves no buildtrees to read.
set(PHYSX_GPU_ARCHIVE_URL
    "https://d4i3qtqj3r0z5.cloudfront.net/PhysXGpu%405.5.0.2aa3c8a3-release-106.4-windows-public.7z")
set(PHYSX_GPU_ARCHIVE_SHA512
    "84f2ba50ae89ebc959d8e35e99750a9fefddd51ba13d0bd96eac08d91b3de658508cb712e4ba253ed2d1be68589e0860747bf0bb324cbb2312574eb686aca06b")

# Where the extracted tree lands. Kept beside the other fetched packages.
set(_physx_gpu_dir "${CMAKE_BINARY_DIR}/_deps/physxgpu")
set(PHYSX_GPU_CHECKED_DLL "${_physx_gpu_dir}/bin/win.x86_64.vc142.mt/checked/PhysXGpu_64.dll")

if(NOT EXISTS "${PHYSX_GPU_CHECKED_DLL}")
  set(_physx_gpu_archive "${_physx_gpu_dir}/PhysXGpu.7z")
  message(STATUS "Fetching PhysX GPU runtime (checked) -- ~239 MB, once")
  file(DOWNLOAD "${PHYSX_GPU_ARCHIVE_URL}" "${_physx_gpu_archive}"
       EXPECTED_HASH SHA512=${PHYSX_GPU_ARCHIVE_SHA512}
       SHOW_PROGRESS STATUS _dl_status)
  list(GET _dl_status 0 _dl_code)
  if(NOT _dl_code EQUAL 0)
    list(GET _dl_status 1 _dl_msg)
    message(FATAL_ERROR "Failed to download PhysXGpu archive: ${_dl_msg}")
  endif()
  # CMake's bundled libarchive reads 7z, so this needs no external 7z program.
  file(ARCHIVE_EXTRACT INPUT "${_physx_gpu_archive}" DESTINATION "${_physx_gpu_dir}")
  if(NOT EXISTS "${PHYSX_GPU_CHECKED_DLL}")
    message(FATAL_ERROR "PhysXGpu archive did not contain the checked DLL: ${PHYSX_GPU_CHECKED_DLL}")
  endif()
  # The other two configurations are ~250 MB each and unused -- release comes from
  # the vcpkg port's own copy, and nothing here builds PhysX's profile config.
  file(REMOVE_RECURSE
       "${_physx_gpu_dir}/bin/win.x86_64.vc142.mt/profile"
       "${_physx_gpu_dir}/bin/win.x86_64.vc142.mt/release"
       "${_physx_gpu_archive}")
endif()

function(physx_stage_gpu_runtime target)
  if(NOT TARGET unofficial::omniverse-physx-sdk::gpu-library
     OR NOT TARGET unofficial::omniverse-physx-sdk::gpu-device-library)
    message(WARNING "PhysX GPU library targets missing; GPU physics will be unavailable")
    return()
  endif()
  # Debug takes the checked DLL, every other configuration the port's release one.
  # PhysXDevice64.dll is a thin shim with no per-config build, so it is shared.
  set(_gpu_dll
      "$<IF:$<CONFIG:Debug>,${PHYSX_GPU_CHECKED_DLL},$<TARGET_FILE:unofficial::omniverse-physx-sdk::gpu-library>>")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_gpu_dll}"
            $<TARGET_FILE:unofficial::omniverse-physx-sdk::gpu-device-library>
            $<TARGET_FILE_DIR:${target}>
    COMMENT "Staging PhysX GPU runtime ($<IF:$<CONFIG:Debug>,checked,release> PhysXGpu_64.dll, PhysXDevice64.dll)"
    VERBATIM)
endfunction()

# Tell the PhysX headers we link the SDK statically. This is NOT optional and the
# port does NOT set it: the generated PxConfig.h defines PX_PHYSX_STATIC_LIB, but
# nothing in the SDK's own headers includes PxConfig.h, and the vcpkg package
# config sets no such define -- so a consumer that just includes <PxPhysicsAPI.h>
# never sees it. Without it, PxFoundationConfig.h (and its siblings) fall through
# to `#define PX_FOUNDATION_API __declspec(dllimport)` and every PhysX entry point
# -- PxCreateFoundation first -- is compiled as an __imp_ import from a DLL that
# does not exist in this static build, and the link (or worse, the first call at
# runtime, depending on the linker's auto-import behavior) falls apart.
target_compile_definitions(PhysX::PhysX INTERFACE PX_PHYSX_STATIC_LIB)
