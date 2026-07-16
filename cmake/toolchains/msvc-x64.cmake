# Hermetic MSVC (cl.exe / link.exe) toolchain for x64 Windows.
#
# The usual way to build with cl.exe is a vcvars64.bat shell, which primes
# INCLUDE/LIB/PATH in the environment. This project instead resolves the MSVC
# toolset and Windows SDK roots at configure time and passes them explicitly on
# every command line. Two reasons:
#   - The flags land in compile_commands.json, so editor IntelliSense works
#     with no developer prompt or environment setup.
#   - Configure and build behave identically from any shell.
#
# cl.exe has no -vctoolsdir/-winsdkdir style flags, so the include roots go in
# as /external:I (an include path whose headers are exempt from /W4 via
# /external:W0 -- the system headers are not this project's warnings to fix)
# and the library roots as explicit /libpath: on the linker, which CMake's
# Ninja generator drives directly (cmake -E vs_link_exe -> link.exe).

include_guard(GLOBAL)

set(_pf86 "$ENV{ProgramFiles\(x86\)}")
set(_pf "$ENV{ProgramFiles}")

# --- MSVC toolset ---------------------------------------------------------------
if(NOT MSVC_TOOLS_DIR)
  find_program(VSWHERE_EXE vswhere
    HINTS "${_pf86}/Microsoft Visual Studio/Installer" "${_pf}/Microsoft Visual Studio/Installer"
    REQUIRED)
  execute_process(
    COMMAND "${VSWHERE_EXE}" -products * -latest
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
            -property installationPath
    OUTPUT_VARIABLE _vs_install
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT _vs_install)
    message(FATAL_ERROR
      "No Visual Studio install carrying the MSVC x64 toolset was found.\n"
      "Install it with:\n"
      "  winget install --id Microsoft.VisualStudio.BuildTools -e --override \""
      "--quiet --wait --norestart --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64\"")
  endif()

  set(_default_ver_file "${_vs_install}/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt")
  if(EXISTS "${_default_ver_file}")
    file(READ "${_default_ver_file}" _msvc_ver)
    string(STRIP "${_msvc_ver}" _msvc_ver)
  else()
    file(GLOB _msvc_vers RELATIVE "${_vs_install}/VC/Tools/MSVC" "${_vs_install}/VC/Tools/MSVC/*")
    list(SORT _msvc_vers COMPARE NATURAL)
    list(POP_BACK _msvc_vers _msvc_ver)
  endif()

  set(MSVC_TOOLS_DIR "${_vs_install}/VC/Tools/MSVC/${_msvc_ver}" CACHE PATH "MSVC toolset root")
endif()

set(_msvc_bin "${MSVC_TOOLS_DIR}/bin/Hostx64/x64")
if(NOT EXISTS "${_msvc_bin}/cl.exe")
  message(FATAL_ERROR "MSVC_TOOLS_DIR has no x64 cl.exe: ${MSVC_TOOLS_DIR}")
endif()
if(NOT EXISTS "${MSVC_TOOLS_DIR}/include/vector")
  message(FATAL_ERROR "MSVC_TOOLS_DIR has no C++ standard library: ${MSVC_TOOLS_DIR}")
endif()

set(CMAKE_C_COMPILER "${_msvc_bin}/cl.exe")
set(CMAKE_CXX_COMPILER "${_msvc_bin}/cl.exe")
set(CMAKE_LINKER "${_msvc_bin}/link.exe")
set(CMAKE_AR "${_msvc_bin}/lib.exe")

# --- Windows SDK --------------------------------------------------------------
if(NOT WINDOWS_SDK_DIR)
  cmake_host_system_information(RESULT _kits_root
    QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
    VALUE "KitsRoot10" VIEW 32 ERROR_VARIABLE _kits_err)
  if(NOT _kits_root)
    set(_kits_root "${_pf86}/Windows Kits/10")
  endif()
  string(REGEX REPLACE "[\\/]+$" "" _kits_root "${_kits_root}")
  set(WINDOWS_SDK_DIR "${_kits_root}" CACHE PATH "Windows 10/11 SDK root")
endif()

if(NOT WINDOWS_SDK_VERSION)
  file(GLOB _sdk_vers RELATIVE "${WINDOWS_SDK_DIR}/Include" "${WINDOWS_SDK_DIR}/Include/10.*")
  # Keep only versions that are actually usable (headers + x64 import libs).
  set(_usable "")
  foreach(_v IN LISTS _sdk_vers)
    if(EXISTS "${WINDOWS_SDK_DIR}/Include/${_v}/um/windows.h"
       AND EXISTS "${WINDOWS_SDK_DIR}/Lib/${_v}/um/x64/kernel32.lib")
      list(APPEND _usable "${_v}")
    endif()
  endforeach()
  if(NOT _usable)
    message(FATAL_ERROR "No usable Windows SDK found under ${WINDOWS_SDK_DIR}")
  endif()
  list(SORT _usable COMPARE NATURAL)
  list(POP_BACK _usable _sdk_ver)
  set(WINDOWS_SDK_VERSION "${_sdk_ver}" CACHE STRING "Windows SDK version")
endif()

set(_sdk_bin "${WINDOWS_SDK_DIR}/bin/${WINDOWS_SDK_VERSION}/x64")
set(CMAKE_RC_COMPILER "${_sdk_bin}/rc.exe")
# vs_link_exe embeds the default manifest with mt.exe; without an explicit path
# CMake would go looking for it in the environment this toolchain avoids needing.
set(CMAKE_MT "${_sdk_bin}/mt.exe")

# --- Wire the roots into every compile and link -------------------------------
set(_tc_includes
  "${MSVC_TOOLS_DIR}/include"
  "${WINDOWS_SDK_DIR}/Include/${WINDOWS_SDK_VERSION}/ucrt"
  "${WINDOWS_SDK_DIR}/Include/${WINDOWS_SDK_VERSION}/shared"
  "${WINDOWS_SDK_DIR}/Include/${WINDOWS_SDK_VERSION}/um"
  "${WINDOWS_SDK_DIR}/Include/${WINDOWS_SDK_VERSION}/winrt")
set(_tc_flags "/external:W0")
foreach(_dir IN LISTS _tc_includes)
  string(APPEND _tc_flags " /external:I \"${_dir}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${_tc_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_tc_flags}")

set(_tc_libpaths
  "/libpath:\"${MSVC_TOOLS_DIR}/lib/x64\""
  "/libpath:\"${WINDOWS_SDK_DIR}/Lib/${WINDOWS_SDK_VERSION}/ucrt/x64\""
  "/libpath:\"${WINDOWS_SDK_DIR}/Lib/${WINDOWS_SDK_VERSION}/um/x64\"")
list(JOIN _tc_libpaths " " _tc_libpaths)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_tc_libpaths}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_tc_libpaths}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_tc_libpaths}")
