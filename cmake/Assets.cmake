# Staging of runtime assets next to the executable.
#
# Models are committed as .glb rather than generated at build time. They are
# produced by tools/gen_models.py, which is not wired into the build on purpose:
# a C++ game should not need a Python interpreter to compile. Run it by hand
# after editing it and commit the result.
#
# The .glb path is spelled out from CMAKE_RUNTIME_OUTPUT_DIRECTORY for the same
# reason add_hlsl_shader does it: add_custom_command's OUTPUT is resolved before
# targets exist, so $<TARGET_FILE_DIR:...> is rejected there.

# stage_assets(<target> DIRECTORY <dir> FILES <name> [<name>...])
#
# Copies each file from <dir> into <exe dir>/<dir>/ and wires it into the build
# graph, so touching an asset re-stages it without a full reconfigure.
function(stage_assets target)
  cmake_parse_arguments(ASSET "" "DIRECTORY" "FILES" ${ARGN})
  if(NOT ASSET_DIRECTORY OR NOT ASSET_FILES)
    message(FATAL_ERROR "stage_assets requires DIRECTORY and FILES")
  endif()
  if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    message(FATAL_ERROR "stage_assets needs CMAKE_RUNTIME_OUTPUT_DIRECTORY set")
  endif()

  set(_staged "")
  foreach(_name IN LISTS ASSET_FILES)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${ASSET_DIRECTORY}/${_name}")
    if(NOT EXISTS "${_src}")
      message(FATAL_ERROR "stage_assets: no such asset: ${_src}")
    endif()
    set(_dst "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${ASSET_DIRECTORY}/${_name}")

    add_custom_command(
      OUTPUT "${_dst}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "${_dst}"
      DEPENDS "${_src}"
      COMMENT "Staging ${ASSET_DIRECTORY}/${_name}"
      VERBATIM)
    list(APPEND _staged "${_dst}")
  endforeach()

  string(MAKE_C_IDENTIFIER "${target}_${ASSET_DIRECTORY}" _asset_target)
  add_custom_target(${_asset_target} DEPENDS ${_staged})
  add_dependencies(${target} ${_asset_target})
endfunction()
