# DirectX 12 Agility SDK, pulled straight from nuget.org.
#
# https://devblogs.microsoft.com/directx/directx12agility/
#
# The Agility SDK decouples the D3D12 runtime from the OS: the app ships
# D3D12Core.dll and opts in by exporting D3D12SDKVersion / D3D12SDKPath (see
# src/agility_exports.cpp). The system d3d12.dll reads those exports and loads
# our redistributable core instead of the one shipped with Windows.
#
# A .nupkg is just a zip, but ExternalProject refuses to extract an unrecognised
# extension, so DOWNLOAD_NAME renames it to .zip on the way in.

include(FetchContent)

set(AGILITY_SDK_VERSION "1.619.4" CACHE STRING "Agility SDK NuGet package version")
# D3D12SDKVersion exported by the executable. This is the minor version of the
# NuGet package: 1.619.4 -> 619. Preview packages additionally require Windows
# Developer Mode to be enabled on the machine running the executable.
set(AGILITY_SDK_INTERFACE_VERSION 619 CACHE STRING "Value exported as D3D12SDKVersion")

FetchContent_Declare(agility_sdk
  URL "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/${AGILITY_SDK_VERSION}/microsoft.direct3d.d3d12.${AGILITY_SDK_VERSION}.nupkg"
  DOWNLOAD_NAME "microsoft.direct3d.d3d12.${AGILITY_SDK_VERSION}.zip"
  URL_HASH SHA512=6a275381027ed758714eedf1ccaeea446b1d9afeddc1f6b6bbc3c85939ef9ffd02b7fae780cd50da635b66b09f5fce99535788551cd64e3663b9e59fe6f7d9de
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(agility_sdk)

set(AGILITY_SDK_INCLUDE_DIR "${agility_sdk_SOURCE_DIR}/build/native/include")
set(AGILITY_SDK_BIN_DIR "${agility_sdk_SOURCE_DIR}/build/native/bin/x64")

# Headers only. The import library (d3d12.lib) still comes from the Windows SDK
# -- Agility replaces the runtime, not the entry points.
add_library(agility_sdk INTERFACE)
add_library(Agility::SDK ALIAS agility_sdk)
target_include_directories(agility_sdk SYSTEM INTERFACE "${AGILITY_SDK_INCLUDE_DIR}")
target_compile_definitions(agility_sdk INTERFACE
  AGILITY_SDK_INTERFACE_VERSION=${AGILITY_SDK_INTERFACE_VERSION})
target_link_libraries(agility_sdk INTERFACE d3d12 dxgi dxguid)

# Stage D3D12Core.dll next to the executable, in the subdirectory named by the
# D3D12SDKPath export. d3d12SDKLayers.dll backs the debug layer and is only
# loaded when debug validation is switched on.
function(agility_sdk_stage_runtime target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${AGILITY_SDK_BIN_DIR}/D3D12Core.dll"
            "${AGILITY_SDK_BIN_DIR}/d3d12SDKLayers.dll"
            "$<TARGET_FILE_DIR:${target}>/D3D12"
    COMMENT "Staging Agility SDK ${AGILITY_SDK_VERSION} runtime into D3D12/"
    VERBATIM)
endfunction()
