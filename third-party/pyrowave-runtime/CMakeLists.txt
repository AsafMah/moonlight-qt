# Builds the PyroWave C API runtime (libpyrowave-shared) from the pinned
# third-party/pyrowave submodule. Moonlight loads this library at runtime and
# hides PyroWave when it is missing, so the app itself never links to it.
#
# Use scripts/build-pyrowave.sh or scripts/build-pyrowave.ps1 rather than
# configuring this project by hand; they fetch the nested submodules and copy
# the result to where packaging expects it.

cmake_minimum_required(VERSION 3.20)
project(moonlight-pyrowave-runtime LANGUAGES C CXX)

if(NOT DEFINED PYROWAVE_SOURCE_DIR)
    cmake_path(SET PYROWAVE_SOURCE_DIR NORMALIZE "${CMAKE_CURRENT_SOURCE_DIR}/../pyrowave")
endif()

# Granite and its Vulkan dependencies are nested submodules. Fail early with a
# useful message instead of letting Granite fall back to other headers.
foreach(required_file IN ITEMS
        "${PYROWAVE_SOURCE_DIR}/CMakeLists.txt"
        "${PYROWAVE_SOURCE_DIR}/pyrowave.h"
        "${PYROWAVE_SOURCE_DIR}/third_party/Granite/CMakeLists.txt"
        "${PYROWAVE_SOURCE_DIR}/third_party/Granite/third_party/volk/volk.h"
        "${PYROWAVE_SOURCE_DIR}/third_party/Granite/third_party/khronos/vulkan-headers/include/vulkan/vulkan.h")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR
            "PyroWave dependency is missing: ${required_file}. "
            "Run scripts/build-pyrowave.sh (or scripts/build-pyrowave.ps1 on Windows), "
            "which initializes the required submodules.")
    endif()
endforeach()

# Granite's CMake compares CMAKE_BUILD_TYPE without quoting, which fails when
# it is empty (the default for Visual Studio generators).
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Keep debugging symbols for the Windows symbol archive (build-arch.bat copies
# every PDB next to the prebuilt DLLs).
if(MSVC)
    add_compile_options(/Zi)
    add_link_options(/DEBUG /OPT:REF /OPT:ICF)
endif()

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(PYROWAVE_DEVEL OFF)
set(PYROWAVE_UTILS OFF)
set(PYROWAVE_BUILD_SHARED ON)
set(PYROWAVE_BUILD_STATIC_C_API OFF)
set(PYROWAVE_BUILD_TESTS OFF)
set(PYROWAVE_INSTALL OFF)
set(GRANITE_INSTALL_TARGETS OFF)
set(GRANITE_INSTALL_EXE_TARGETS OFF)
set(GRANITE_RENDERDOC_CAPTURE OFF)

add_subdirectory("${PYROWAVE_SOURCE_DIR}" pyrowave EXCLUDE_FROM_ALL)

# Put the runtime in one predictable directory for every generator. The
# generator expression stops multi-config generators from adding a
# per-configuration subdirectory.
set(PYROWAVE_RUNTIME_OUTPUT_DIR "${CMAKE_BINARY_DIR}/out")
set_target_properties(pyrowave-shared PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${PYROWAVE_RUNTIME_OUTPUT_DIR}>"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${PYROWAVE_RUNTIME_OUTPUT_DIR}>"
    ARCHIVE_OUTPUT_DIRECTORY "$<1:${PYROWAVE_RUNTIME_OUTPUT_DIR}/lib>"
    PDB_OUTPUT_DIRECTORY "$<1:${PYROWAVE_RUNTIME_OUTPUT_DIR}>")
