# NVIDIA Flow (GameWorks 1.0.1), the sparse-grid GPU fluid sim behind the grill's
# smoke and coal fire.
#
# https://github.com/NVIDIAGameWorks/Flow  (NVIDIA Source Code License, 1-Way Commercial)
#
# Two generations of Flow exist and only this one fits a raw-D3D12 game. The modern
# "Flow 2" (omni.flowsdk, shipped inside NVIDIA-Omniverse/PhysX) has no public D3D12
# device backend -- its NvFlowGetDeviceInterface wires up only Vulkan and CPU, so a
# D3D12 app would have to stand up a whole Vulkan device and share textures across the
# API boundary. This original GameWorks Flow is D3D12-native: NvFlowCreateContextD3D12
# consumes our existing ID3D12Device / queue / fence / command list and records into
# *our* command list, and NvFlowVolumeRender composites its ray-march straight into an
# app render target with an app depth buffer for occlusion (see src/flow_volume.cpp).
#
# It ships as a prebuilt DLL + import lib -- nothing to build from source. Rather than
# vendor those binaries in the tree, they are fetched once at configure time from a
# pinned, hash-verified GitHub source tarball, the same fetch-a-pinned-binary pattern
# PhysX.cmake uses for the PhysXGpu runtime and AgilitySDK.cmake for the Agility SDK.

# The repository state the binaries and headers are taken from. Pinned by commit so the
# fetched tree never shifts under the build; the archive is hash-checked below.
set(FLOW_COMMIT "5f7db82d72bb7427ea09d14b50aaed276e553632")
set(FLOW_ARCHIVE_URL
    "https://github.com/NVIDIAGameWorks/Flow/archive/${FLOW_COMMIT}.tar.gz")
set(FLOW_ARCHIVE_SHA512
    "b12923f9665269abc6b81a6b5636602fd8056e6d45c38d099c8cad794856b576fa1e61b3eb08d478c36238611747a1b76598ee8c1a8581c70da969976ab000ae")

# Where the extracted tree lands, beside the other fetched packages. GitHub's source
# tarball unpacks to a single top-level "Flow-<commit>" directory.
set(_flow_deps "${CMAKE_BINARY_DIR}/_deps/flow")
set(FLOW_ROOT "${_flow_deps}/Flow-${FLOW_COMMIT}")

if(NOT EXISTS "${FLOW_ROOT}/include/NvFlowContextD3D12.h")
  set(_flow_archive "${_flow_deps}/flow.tar.gz")
  message(STATUS "Fetching NVIDIA Flow (GameWorks 1.0.1) -- ~31 MB, once")
  file(DOWNLOAD "${FLOW_ARCHIVE_URL}" "${_flow_archive}"
       EXPECTED_HASH SHA512=${FLOW_ARCHIVE_SHA512}
       SHOW_PROGRESS STATUS _flow_dl_status)
  list(GET _flow_dl_status 0 _flow_dl_code)
  if(NOT _flow_dl_code EQUAL 0)
    list(GET _flow_dl_status 1 _flow_dl_msg)
    message(FATAL_ERROR "Failed to download NVIDIA Flow archive: ${_flow_dl_msg}")
  endif()
  # CMake's bundled libarchive reads .tar.gz, so this needs no external tools.
  file(ARCHIVE_EXTRACT INPUT "${_flow_archive}" DESTINATION "${_flow_deps}")
  if(NOT EXISTS "${FLOW_ROOT}/include/NvFlowContextD3D12.h")
    message(FATAL_ERROR "NVIDIA Flow archive did not extract to the expected layout: ${FLOW_ROOT}")
  endif()
  file(REMOVE "${_flow_archive}")
endif()

set(_flow_inc "${FLOW_ROOT}/include")
set(_flow_lib "${FLOW_ROOT}/lib/win64")

# The library is only built for x64, and this project is x64-only anyway. Two builds
# ship: the ~1.8 MB Release library and a ~20 MB Debug one carrying the D3D12 debug
# layer's chattier validation. Map the game's Debug config to the Debug library so its
# diagnostics reach the debugger -- the same Debug/Release split Fmod.cmake makes for
# fmod vs fmodL -- and Release (and any other config) to the shipping build.
#
# Because IMPORTED_LOCATION_<CONFIG> is set, $<TARGET_FILE:Flow::Flow> resolves to the
# right DLL per config, which flow_stage_runtime below relies on. The unsuffixed
# IMPORTED_LOCATION is the fallback for configs other than Debug.
add_library(Flow::Flow SHARED IMPORTED GLOBAL)
set_target_properties(Flow::Flow PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_flow_inc}"
  IMPORTED_CONFIGURATIONS "Debug;Release"
  IMPORTED_LOCATION_DEBUG   "${_flow_lib}/NvFlowLibDebug_win64.dll"
  IMPORTED_IMPLIB_DEBUG     "${_flow_lib}/NvFlowLibDebug_win64.lib"
  IMPORTED_LOCATION_RELEASE "${_flow_lib}/NvFlowLibRelease_win64.dll"
  IMPORTED_IMPLIB_RELEASE   "${_flow_lib}/NvFlowLibRelease_win64.lib"
  IMPORTED_LOCATION         "${_flow_lib}/NvFlowLibRelease_win64.dll"
  IMPORTED_IMPLIB           "${_flow_lib}/NvFlowLibRelease_win64.lib")

# Stage the Flow runtime DLL next to the executable, plus the licence text the library's
# terms require be distributed with it. $<TARGET_FILE:Flow::Flow> picks the Debug DLL in
# Debug and the Release one otherwise, matching the import library the target links.
function(flow_stage_runtime target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Flow::Flow>"
            "${FLOW_ROOT}/LICENSE.txt"
            "$<TARGET_FILE_DIR:${target}>"
    COMMENT "Staging NVIDIA Flow runtime ($<TARGET_FILE_NAME:Flow::Flow>)"
    VERBATIM)
endfunction()
