# Shader Model 6.x compilation via the redistributable DirectX Shader Compiler.
#
# The Windows SDK ships a dxc.exe, but it lags the standalone releases. Pulling
# DXC from nuget.org keeps the shader compiler versioned with the project the
# same way the Agility SDK keeps the runtime versioned with it.
#
# Shaders are compiled ahead of time into .cso blobs. dxil.dll signs them at
# build time, so nothing from this package needs to ship with the game.

include(FetchContent)

set(DXC_VERSION "1.9.2602.24" CACHE STRING "DirectX Shader Compiler NuGet package version")

FetchContent_Declare(dxc
  URL "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.dxc/${DXC_VERSION}/microsoft.direct3d.dxc.${DXC_VERSION}.nupkg"
  DOWNLOAD_NAME "microsoft.direct3d.dxc.${DXC_VERSION}.zip"
  URL_HASH SHA512=354182aa58f528d5138ff2bfc97fd48cd1cfe8d0fcff146830d35d7630e73e4bb1db3aa39b0e807dfe905bf13d4b96229f9385ac14d562831dbfdd0d28eafb05
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(dxc)

set(DXC_EXECUTABLE "${dxc_SOURCE_DIR}/build/native/bin/x64/dxc.exe")

# add_hlsl_shader(<target> SOURCE <file.hlsl> ENTRY <fn> PROFILE <vs_6_6|ps_6_6|...> [OUTPUT <name.cso>])
#
# Compiles one entry point and drops the .cso into <exe dir>/shaders/. Adds a
# depfile so edits to #included .hlsli files trigger a rebuild.
#
# The .cso path is spelled out from CMAKE_RUNTIME_OUTPUT_DIRECTORY rather than
# $<TARGET_FILE_DIR:...>: add_custom_command's OUTPUT is resolved before targets
# exist, so target-dependent generator expressions are rejected there.
function(add_hlsl_shader target)
  cmake_parse_arguments(SHADER "" "SOURCE;ENTRY;PROFILE;OUTPUT" "" ${ARGN})
  if(NOT SHADER_SOURCE OR NOT SHADER_ENTRY OR NOT SHADER_PROFILE)
    message(FATAL_ERROR "add_hlsl_shader requires SOURCE, ENTRY and PROFILE")
  endif()
  if(NOT SHADER_OUTPUT)
    get_filename_component(_stem "${SHADER_SOURCE}" NAME_WE)
    set(SHADER_OUTPUT "${_stem}.${SHADER_ENTRY}.cso")
  endif()
  if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    message(FATAL_ERROR "add_hlsl_shader needs CMAKE_RUNTIME_OUTPUT_DIRECTORY set")
  endif()

  get_filename_component(_src "${SHADER_SOURCE}" ABSOLUTE)
  set(_out_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders")
  set(_out "${_out_dir}/${SHADER_OUTPUT}")
  # Multi-config builds compile each shader once per configuration, so the
  # depfile has to be per-configuration too.
  set(_depfile_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders/$<CONFIG>")
  set(_depfile "${_depfile_dir}/${SHADER_OUTPUT}.d")

  # -Zi embeds debug info for PIX / RenderDoc; -Qembed_debug keeps it in the
  # blob rather than emitting a separate .pdb we would have to ship.
  #
  # One generator expression per flag: semicolons inside a single $<IF:...> are
  # taken as list separators and tear the expression apart before it evaluates.
  # Arguments that evaluate to the empty string are dropped from the command.
  set(_flags
    $<$<CONFIG:Debug>:-Od>
    $<$<CONFIG:Debug>:-Zi>
    $<$<CONFIG:Debug>:-Qembed_debug>
    $<$<NOT:$<CONFIG:Debug>>:-O3>)

  # Two invocations on purpose: given -MF, dxc emits the dependency list and
  # skips code generation entirely, so a single call that also passes -Fo exits
  # 0 having written no .cso at all. Scan first, then compile for real.
  add_custom_command(
    OUTPUT "${_out}"
    # dxc writes the depfile itself and will not create its parent directory.
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_out_dir}" "${_depfile_dir}"
    COMMAND "${DXC_EXECUTABLE}"
            -T ${SHADER_PROFILE} -E ${SHADER_ENTRY}
            -MF "${_depfile}" "${_src}"
    COMMAND "${DXC_EXECUTABLE}"
            -T ${SHADER_PROFILE} -E ${SHADER_ENTRY}
            ${_flags}
            -WX -Ges
            -Fo "${_out}" "${_src}"
    DEPENDS "${_src}" "${DXC_EXECUTABLE}"
    DEPFILE "${_depfile}"
    COMMENT "dxc ${SHADER_PROFILE} ${SHADER_ENTRY} <- ${SHADER_SOURCE}"
    VERBATIM COMMAND_EXPAND_LISTS)

  # One synthetic target per shader keeps the .cso wired into the build graph.
  string(MAKE_C_IDENTIFIER "${target}_${SHADER_OUTPUT}" _shader_target)
  add_custom_target(${_shader_target} DEPENDS "${_out}")
  add_dependencies(${target} ${_shader_target})
endfunction()
