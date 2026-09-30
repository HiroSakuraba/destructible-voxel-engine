# Single version source for DVE (packaging plan §4.4).
#
# project(... VERSION x.y.z) in the top-level CMakeLists.txt is the only place the version is
# written. This module turns it into:
#   - ${DVE_GENERATED_INCLUDE_DIR}/dve/version.hpp (DVE_VERSION_* macros + dve::kVersion*),
#     installed with the headers and used by dve_player --version;
#   - DVE_GIT_DESCRIBE (git describe --always --dirty at configure time, blank without git);
#   - DVE_PUBLIC_INCLUDE_DIRS, the include-directory generator expression every public DVE
#     library uses so install(EXPORT) can relocate it.
# The dve_version_consistency test checks release-manifest.json and README.md against it.
include_guard(GLOBAL)

set(DVE_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/generated/include")
set(DVE_PUBLIC_INCLUDE_DIRS
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

set(DVE_GIT_DESCRIBE "" CACHE STRING "Override the git describe string baked into dve/version.hpp (blank = ask git)")
set(_dve_git_describe "${DVE_GIT_DESCRIBE}")
if(NOT _dve_git_describe)
    find_package(Git QUIET)
    if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --always --dirty
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            OUTPUT_VARIABLE _dve_git_describe
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
endif()
if(NOT _dve_git_describe MATCHES "^[A-Za-z0-9._+-]*$")
    set(_dve_git_describe "")
endif()
set(DVE_VERSION_GIT_DESCRIBE "${_dve_git_describe}")

configure_file("${CMAKE_CURRENT_LIST_DIR}/version.hpp.in"
    "${DVE_GENERATED_INCLUDE_DIR}/dve/version.hpp" @ONLY)
message(STATUS "DVE version ${PROJECT_VERSION} (git: ${DVE_VERSION_GIT_DESCRIBE})")
