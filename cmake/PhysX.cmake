# NVIDIA PhysX 5, via the vcpkg "physx" port (Omniverse PhysX SDK 5.5.0).
#
# https://github.com/microsoft/vcpkg/tree/master/ports/physx  (port)
# https://github.com/NVIDIA-Omniverse/PhysX                    (BSD-3-Clause)
#
# PhysX has no clean FetchContent/add_subdirectory story of its own -- its build
# is driven by an in-repo generate_projects + packman bootstrap that fights this
# project's clang-cl / Ninja / VS2026 toolchain. The vcpkg port wraps all of that
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
# The port is built by vcpkg with MSVC; the game is built with clang-cl. Both
# target the MSVC ABI and CRT, so the static libraries link cleanly into the
# clang-cl executable.

find_package(unofficial-omniverse-physx-sdk CONFIG REQUIRED)

# The rest of the build links one PhysX::PhysX target and stays agnostic about how
# PhysX is sourced. The port's target already carries the include dirs, the
# PX_PHYSX_STATIC_LIB define and the full set of static libraries.
add_library(PhysX::PhysX INTERFACE IMPORTED GLOBAL)
target_link_libraries(PhysX::PhysX INTERFACE unofficial::omniverse-physx-sdk::sdk)

# Consume with clang-cl. PhysX's SSE vec-math header (PxVecMathSSE.h) reads
# MSVC-specific named members of __m128 (.m128_u16, ...). clang-cl defines
# _MSC_VER, so PhysX takes that MSVC path -- but clang-cl's __m128 is a plain
# vector type with no such members, and the header fails to compile in consuming
# code. PX_SIMD_DISABLED switches PhysX to its scalar vec-math header instead. It
# only changes inline helpers this project never calls (the Vec4V family never
# crosses the public API we use), so the prebuilt library's ABI is unaffected.
target_compile_definitions(PhysX::PhysX INTERFACE PX_SIMD_DISABLED)
