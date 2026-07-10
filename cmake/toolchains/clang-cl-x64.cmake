# Hermetic clang-cl / lld-link toolchain for x64 Windows.
#
# clang's built-in MSVC detection only probes Visual Studio 8/9/10 era paths and
# does not find Visual Studio 18 (2026), so it locates no C++ standard library.
# Rather than priming the shell with vcvars64.bat, every root is discovered here
# and passed explicitly on the command line. Those flags then land in
# compile_commands.json, which is what lets clangd work with no environment.

include_guard(GLOBAL)

set(_pf86 "$ENV{ProgramFiles\(x86\)}")
set(_pf "$ENV{ProgramFiles}")

# --- LLVM ---------------------------------------------------------------------
find_program(LLVM_CLANG_CL clang-cl
  HINTS "${_pf}/LLVM/bin" "${_pf86}/LLVM/bin" "$ENV{LLVM_ROOT}/bin"
  REQUIRED)
get_filename_component(LLVM_BIN_DIR "${LLVM_CLANG_CL}" DIRECTORY)

set(CMAKE_C_COMPILER "${LLVM_CLANG_CL}")
set(CMAKE_CXX_COMPILER "${LLVM_CLANG_CL}")
set(CMAKE_LINKER "${LLVM_BIN_DIR}/lld-link.exe")
set(CMAKE_AR "${LLVM_BIN_DIR}/llvm-lib.exe")
set(CMAKE_RC_COMPILER "${LLVM_BIN_DIR}/llvm-rc.exe")

# --- MSVC toolset (STL headers + import libs; no compiler used) ----------------
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
      "clang-cl needs MSVC's C++ standard library. Install it with:\n"
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

if(NOT EXISTS "${MSVC_TOOLS_DIR}/include/vector")
  message(FATAL_ERROR "MSVC_TOOLS_DIR has no C++ standard library: ${MSVC_TOOLS_DIR}")
endif()

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

# --- Wire the roots into every compile and link -------------------------------
set(_tc_flags "-vctoolsdir \"${MSVC_TOOLS_DIR}\" -winsdkdir \"${WINDOWS_SDK_DIR}\" -winsdkversion ${WINDOWS_SDK_VERSION}")
set(CMAKE_C_FLAGS_INIT "${_tc_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_tc_flags}")

# CMake drives lld-link directly rather than through the clang driver, so the
# driver's implicit /libpath: arguments never get generated. Supply them here.
set(_tc_libpaths
  "/libpath:\"${MSVC_TOOLS_DIR}/lib/x64\""
  "/libpath:\"${WINDOWS_SDK_DIR}/Lib/${WINDOWS_SDK_VERSION}/ucrt/x64\""
  "/libpath:\"${WINDOWS_SDK_DIR}/Lib/${WINDOWS_SDK_VERSION}/um/x64\"")
list(JOIN _tc_libpaths " " _tc_libpaths)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_tc_libpaths}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_tc_libpaths}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_tc_libpaths}")
