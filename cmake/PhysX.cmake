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
# Static libraries plus a CPU-only scene mean nothing has to be staged next to the
# executable -- no PhysX DLLs, no GPU runtime (we never link ::gpu-library).
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
# to the executable. The port ships only a release build of them; its package
# config points both configurations at that one file, which is intended -- the
# boundary between app and GPU library is a C interface, not a CRT handoff.
function(physx_stage_gpu_runtime target)
  if(NOT TARGET unofficial::omniverse-physx-sdk::gpu-library
     OR NOT TARGET unofficial::omniverse-physx-sdk::gpu-device-library)
    message(WARNING "PhysX GPU library targets missing; GPU physics will be unavailable")
    return()
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:unofficial::omniverse-physx-sdk::gpu-library>
            $<TARGET_FILE:unofficial::omniverse-physx-sdk::gpu-device-library>
            $<TARGET_FILE_DIR:${target}>
    COMMENT "Staging PhysX GPU runtime (PhysXGpu_64.dll, PhysXDevice64.dll)"
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
