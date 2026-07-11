# FMOD Core, from a locally installed FMOD Engine SDK.
#
# https://www.fmod.com/docs/2.03/api/core-api.html
#
# Unlike every other dependency in this project, FMOD is NOT built from source
# and NOT fetched at configure time. It is proprietary, ships as prebuilt DLLs
# under a click-through licence, and is not redistributable the way the vcpkg and
# FetchContent packages are. So the FMOD Engine SDK has to be installed on the
# machine (from fmod.com) and this module simply points at it -- there is nothing
# here to compile.
#
# Only the Core API is used: the low-level playback and 3D-positioning layer, a
# direct analogue of the XAudio2/X3DAudio wrapper it replaces. FMOD Studio (the
# event/bank system and its separate authoring tool) is deliberately not used --
# the game has exactly one looping, distance-attenuated sound.

# Default to the Windows installer's location; override with -DFMOD_DIR=<root>
# for a vendored copy or a non-standard install. The root is the folder that
# contains api/core.
set(FMOD_DIR "C:/Program Files (x86)/FMOD SoundSystem/FMOD Studio API Windows"
    CACHE PATH "Root of an installed FMOD Engine SDK (the folder containing api/core)")

set(_fmod_inc "${FMOD_DIR}/api/core/inc")
set(_fmod_lib "${FMOD_DIR}/api/core/lib/x64")

if(NOT EXISTS "${_fmod_inc}/fmod.hpp")
  message(FATAL_ERROR
    "FMOD Core headers not found under '${_fmod_inc}'.\n"
    "Install the FMOD Engine SDK, or configure with -DFMOD_DIR=<sdk root> "
    "(the folder that contains api/core).")
endif()

# fmod.dll is the shipping runtime; fmodL.dll is the same runtime with logging
# compiled in. Map the game's Debug config to the logging build so FMOD's own
# diagnostics reach the debugger -- the analogue of the old AudioEngine_Debug
# flag -- and Release (and any other config) to the quiet shipping build.
#
# Because IMPORTED_LOCATION_<CONFIG> is set, $<TARGET_FILE:fmod::core> resolves
# to the right DLL per config, which the staging function below relies on. The
# unsuffixed IMPORTED_LOCATION is the fallback for configs other than Debug.
add_library(fmod::core SHARED IMPORTED GLOBAL)
set_target_properties(fmod::core PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_fmod_inc}"
  IMPORTED_CONFIGURATIONS "Debug;Release"
  IMPORTED_LOCATION_DEBUG   "${_fmod_lib}/fmodL.dll"
  IMPORTED_IMPLIB_DEBUG     "${_fmod_lib}/fmodL_vc.lib"
  IMPORTED_LOCATION_RELEASE "${_fmod_lib}/fmod.dll"
  IMPORTED_IMPLIB_RELEASE   "${_fmod_lib}/fmod_vc.lib"
  IMPORTED_LOCATION         "${_fmod_lib}/fmod.dll"
  IMPORTED_IMPLIB           "${_fmod_lib}/fmod_vc.lib")

# Stage the FMOD runtime DLL next to the executable. $<TARGET_FILE:fmod::core>
# picks fmodL.dll in Debug and fmod.dll otherwise, matching the import library
# the target links against -- so the DLL beside the exe always matches the .lib.
function(fmod_stage_runtime target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:fmod::core>"
            "$<TARGET_FILE_DIR:${target}>"
    COMMENT "Staging FMOD runtime ($<TARGET_FILE_NAME:fmod::core>)"
    VERBATIM)
endfunction()
