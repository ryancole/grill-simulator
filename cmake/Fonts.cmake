# MSDF font atlas baking via a pinned, prebuilt msdf-atlas-gen.
#
# https://github.com/Chlumsky/msdf-atlas-gen
#
# The game draws HUD text from a multi-channel signed-distance-field atlas: one
# small texture whose texels store the distance to the nearest glyph edge, so the
# pixel shader can render the text crisp at any size, distance or rotation. The
# atlas is baked from a .ttf ahead of time -- msdf-atlas-gen is a build tool, not
# a runtime or link dependency, and nothing from it ships with the game.
#
# This treats the font baker exactly the way Shaders.cmake treats the shader
# compiler: fetch a prebuilt, redistributable executable from an official release,
# pinned by hash, and run it in a build rule. A clean checkout reproduces the
# atlas byte-for-byte with nothing to install by hand -- the .ttf is committed as
# the source, and the .png/.csv it bakes into live in the build tree, never the
# repo. Re-baking is a rebuild, not a chore.

include(FetchContent)

set(MSDF_ATLAS_GEN_VERSION "1.4" CACHE STRING "msdf-atlas-gen release tag, without the leading v")

FetchContent_Declare(msdf_atlas_gen
  URL "https://github.com/Chlumsky/msdf-atlas-gen/releases/download/v${MSDF_ATLAS_GEN_VERSION}/msdf-atlas-gen-${MSDF_ATLAS_GEN_VERSION}-win64.zip"
  DOWNLOAD_NAME "msdf-atlas-gen-${MSDF_ATLAS_GEN_VERSION}-win64.zip"
  URL_HASH SHA512=e265b6011562237c8e69876be4be91b9c6aa7e7e7ecccb03aaad5cb67b3522ce2ac052b781178e09e5d0ff2cecdb5536fa7519ebdf184efe3110f85cb1486879
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(msdf_atlas_gen)

# FetchContent flattens the archive's single top-level directory, so the exe
# lands directly in the source dir rather than under a msdf-atlas-gen/ subfolder.
set(MSDF_ATLAS_GEN_EXECUTABLE "${msdf_atlas_gen_SOURCE_DIR}/msdf-atlas-gen.exe")

# bake_font_atlas(<target> FONT <file.ttf> IMAGE <name.png> CSV <name.csv>
#                 [SIZE <em px>] [PXRANGE <px>])
#
# Bakes an MSDF atlas and its glyph-metrics CSV into <exe dir>/assets/fonts/. The
# two outputs are wired into the build graph, so editing the .ttf re-bakes them.
#
# SIZE is the minimum em size in the atlas; the tool grows it to the largest that
# still fits the chosen dimensions. PXRANGE is the width of the distance field in
# atlas pixels -- it must match kDistanceRange in src/font.hpp, which is how the
# pixel shader turns the sampled distance into a crisp edge. Charset defaults to
# printable ASCII, which is every glyph the HUD needs.
#
# -yorigin top puts the atlas origin at the top-left, matching Direct3D's texture
# space, so the baked atlasBounds are read straight into UVs with no vertical flip.
function(bake_font_atlas target)
  cmake_parse_arguments(FONT "" "FONT;IMAGE;CSV;SIZE;PXRANGE" "" ${ARGN})
  if(NOT FONT_FONT OR NOT FONT_IMAGE OR NOT FONT_CSV)
    message(FATAL_ERROR "bake_font_atlas requires FONT, IMAGE and CSV")
  endif()
  if(NOT FONT_SIZE)
    set(FONT_SIZE 48)
  endif()
  if(NOT FONT_PXRANGE)
    set(FONT_PXRANGE 4)
  endif()
  if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    message(FATAL_ERROR "bake_font_atlas needs CMAKE_RUNTIME_OUTPUT_DIRECTORY set")
  endif()

  get_filename_component(_font "${FONT_FONT}" ABSOLUTE)
  set(_out_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets/fonts")
  set(_image "${_out_dir}/${FONT_IMAGE}")
  set(_csv "${_out_dir}/${FONT_CSV}")

  add_custom_command(
    OUTPUT "${_image}" "${_csv}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_out_dir}"
    COMMAND "${MSDF_ATLAS_GEN_EXECUTABLE}"
            -font "${_font}"
            -type msdf -format png
            -size ${FONT_SIZE} -pxrange ${FONT_PXRANGE}
            -yorigin top -pots
            -imageout "${_image}" -csv "${_csv}"
    DEPENDS "${_font}" "${MSDF_ATLAS_GEN_EXECUTABLE}"
    COMMENT "msdf-atlas-gen ${FONT_IMAGE} <- ${FONT_FONT}"
    VERBATIM)

  string(MAKE_C_IDENTIFIER "${target}_${FONT_IMAGE}" _font_target)
  add_custom_target(${_font_target} DEPENDS "${_image}" "${_csv}")
  add_dependencies(${target} ${_font_target})
endfunction()
