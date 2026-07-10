# fastgltf, a glTF 2.0 parser, pulled straight from GitHub.
#
# https://github.com/spnda/fastgltf
#
# fastgltf parses the JSON with simdjson and hands back accessors we can iterate
# straight into DirectXMath types -- it ships ElementTraits for XMFLOAT2/3/4 and
# XMFLOAT4X4 in <fastgltf/dxmath_element_traits.hpp>, so no vector maths library
# comes along for the ride.
#
# fastgltf's own cmake/dependencies.cmake reaches for simdjson with a bare
# file(DOWNLOAD) and no hash. This project pins every fetched byte, so the two
# single-header files are downloaded here, by hash, into the exact path fastgltf
# looks in. It finds them, sees a version no older than the one it wants, and
# skips its own download entirely.

include(FetchContent)

set(FASTGLTF_VERSION "0.9.0" CACHE STRING "fastgltf release tag, without the leading v")
# Must match SIMDJSON_TARGET_VERSION in fastgltf's cmake/dependencies.cmake. If
# fastgltf raises it, it re-downloads over these files -- unpinned -- so raise
# this and the hashes below at the same time.
set(SIMDJSON_VERSION "3.12.3" CACHE STRING "simdjson single-header version fastgltf expects")

# Populate only. MakeAvailable skips add_subdirectory when SOURCE_SUBDIR names a
# directory with no CMakeLists.txt, which is what lets simdjson be dropped into
# the source tree before fastgltf's own configure runs.
FetchContent_Declare(fastgltf
  URL "https://github.com/spnda/fastgltf/archive/refs/tags/v${FASTGLTF_VERSION}.tar.gz"
  URL_HASH SHA512=b18162eb8a1631d9a28ed97961ac8f08d6aa2797f2bf035a470660cfd052f25c2bd47b77ce2c3f5367d5006c706cf6e00a710c14a25ad5e02b619430ea076882
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR does-not-exist)
FetchContent_MakeAvailable(fastgltf)

set(_simdjson_dir "${fastgltf_SOURCE_DIR}/deps/simdjson")
set(_simdjson_url "https://raw.githubusercontent.com/simdjson/simdjson/v${SIMDJSON_VERSION}/singleheader")

# file(DOWNLOAD) with EXPECTED_HASH is a no-op when the file already matches, so
# this costs one hash of each file per configure and nothing else.
file(DOWNLOAD "${_simdjson_url}/simdjson.h" "${_simdjson_dir}/simdjson.h"
  EXPECTED_HASH SHA512=3dc417191d0f0f8334dda9f178b19587110c04cf35b0a8a3c66b0dcfc9b778aab1e9cd3b697d42124da32fc3c9bc63c7c74bbad60f975aed3807a12ee64fcfdc
  TLS_VERIFY ON)
file(DOWNLOAD "${_simdjson_url}/simdjson.cpp" "${_simdjson_dir}/simdjson.cpp"
  EXPECTED_HASH SHA512=95bad896ee16162dd457aef0e616bb61c4359cdd0ddf34462a968ed9eb06cdc5d217dccad531875275b44a5d47061a653285bb19aad44a4e96e6b92d2bdfa8ef
  TLS_VERIFY ON)

# Everything off. Tests and examples are what drag in glm, Catch2, GLFW and imgui;
# with them off, simdjson above is the whole dependency graph.
set(FASTGLTF_ENABLE_TESTS OFF)
set(FASTGLTF_ENABLE_EXAMPLES OFF)
set(FASTGLTF_ENABLE_DOCS OFF)
set(FASTGLTF_ENABLE_CPP_MODULES OFF)

add_subdirectory("${fastgltf_SOURCE_DIR}" "${fastgltf_BINARY_DIR}" EXCLUDE_FROM_ALL)

# fastgltf and simdjson are third-party code compiled with our /W4. Their headers
# are pulled into our translation units too, so mark both the target's own
# warnings and its interface includes as somebody else's problem.
get_target_property(_fastgltf_includes fastgltf INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(fastgltf PROPERTIES
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_fastgltf_includes}")
target_compile_options(fastgltf PRIVATE -w)
