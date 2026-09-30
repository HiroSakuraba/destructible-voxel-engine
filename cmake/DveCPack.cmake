# CPack configuration (packaging plan §4.3). Package names follow decision D7:
#   dve-runtime  dve_player (+ bundled non-system libraries in archives)
#   dve-editor   dve_desktop_editor / dve_native_editor_x11 + share/dve/assets (no sample maps, D8)
#   dve-tools    dve_pack, dve_cook_*, dve_asset_index, dve_prefab_tool
#   dve-dev      headers, static libraries, lib/cmake/dve
# Linux: TGZ + DEB (`cpack -G "TGZ;DEB"` in the build folder). Windows: ZIP + NSIS, configured
# but untested (no Windows runner yet). The version is PROJECT_VERSION (see cmake/DveVersion.cmake).
#
# The engine is MIT-licensed (root LICENSE, decision D1). CPACK_RESOURCE_FILE_LICENSE points at it
# (shown by the NSIS installer), and cmake/DveLicense.cmake installs it into every package as
# share/doc/dve-<group>/LICENSE plus a Debian copyright file (/usr/share/doc/dve-<group>/copyright).
include_guard(GLOBAL)
if(NOT DVE_INSTALL)
    return()
endif()

set(CPACK_PACKAGE_NAME "dve")
set(CPACK_PACKAGE_VENDOR "HiroSakuraba")
set(CPACK_PACKAGE_CONTACT "HiroSakuraba <52479046+HiroSakuraba@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Destructible Voxel Engine: game player, editor, tools and SDK")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/HiroSakuraba/destructible-voxel-engine")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "DVE ${PROJECT_VERSION}")
set(CPACK_RESOURCE_FILE_README "${PROJECT_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_LICENSE "${DVE_LICENSE_FILE}")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_STRIP_FILES ON)
set(CPACK_THREADS 0)
set(CPACK_SOURCE_IGNORE_FILES "/\\\\.git/" "/out/" "/build/")

if(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
else()
    set(CPACK_GENERATOR "TGZ;DEB")
endif()

# Components -> groups -> one archive / .deb per group.
set(CPACK_COMPONENTS_ALL ${DVE_INSTALLED_COMPONENTS})
set(CPACK_COMPONENTS_GROUPING ONE_PER_GROUP)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY ON)

set(_dve_groups runtime editor tools dev)
set(_dve_group_runtime Runtime RuntimeDeps)
set(_dve_group_editor Editor EditorDeps)
set(_dve_group_tools Tools ToolsDeps)
set(_dve_group_dev Development)
set(_dve_summary_runtime "DVE game player (dve_player)")
set(_dve_summary_editor "DVE desktop editor and editor assets")
set(_dve_summary_tools "DVE content tools (dve_pack, asset cookers)")
set(_dve_summary_dev "DVE SDK: headers, static libraries and the dve CMake package")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(_dve_arch x86_64)
else()
    set(_dve_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
# Every archive unpacks into the same dve-<version>-<system>-<arch>/ folder, so extracting
# several of them (runtime + tools + dev, ...) yields one merged install tree.
set(CPACK_PACKAGE_FILE_NAME "dve-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${_dve_arch}")
foreach(_dve_group IN LISTS _dve_groups)
    string(TOUPPER "${_dve_group}" _dve_GROUP)
    set(CPACK_COMPONENT_GROUP_${_dve_GROUP}_DISPLAY_NAME "dve-${_dve_group}")
    set(CPACK_COMPONENT_GROUP_${_dve_GROUP}_DESCRIPTION "${_dve_summary_${_dve_group}}")
    foreach(_dve_component IN LISTS _dve_group_${_dve_group})
        string(TOUPPER "${_dve_component}" _dve_COMPONENT)
        set(CPACK_COMPONENT_${_dve_COMPONENT}_GROUP "${_dve_group}")
    endforeach()
    set(CPACK_ARCHIVE_${_dve_GROUP}_FILE_NAME
        "dve-${_dve_group}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${_dve_arch}")
    set(CPACK_DEBIAN_${_dve_GROUP}_PACKAGE_NAME "dve-${_dve_group}")
    set(CPACK_DEBIAN_${_dve_GROUP}_DESCRIPTION "${_dve_summary_${_dve_group}}")
endforeach()
set(CPACK_COMPONENT_RUNTIMEDEPS_HIDDEN ON)
set(CPACK_COMPONENT_EDITORDEPS_HIDDEN ON)
set(CPACK_COMPONENT_TOOLSDEPS_HIDDEN ON)

# DEB: installs into /usr and depends on distribution libraries instead of bundling them.
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "games")
set(CPACK_DEBIAN_DEV_PACKAGE_SECTION "libdevel")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_COMPRESSION_TYPE "xz")
find_program(DVE_DPKG_SHLIBDEPS dpkg-shlibdeps)
if(DVE_DPKG_SHLIBDEPS)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    # The bundled lib/dve folder is not part of the .deb, so let shlibdeps resolve from /usr.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "")
else()
    # Without dpkg-dev, list the obvious runtime dependencies by hand (the C/C++ runtime; SDL3
    # and Lua when linked dynamically). Install dpkg-dev for exact, versioned Depends.
    set(_dve_hand_depends "libc6, libstdc++6, libgcc-s1")
    if(DVE_SDL3_TARGET AND NOT DVE_SDL3_FETCHED)
        string(APPEND _dve_hand_depends ", libsdl3-0")
    endif()
    if(DVE_ENABLE_LUA)
        string(APPEND _dve_hand_depends ", liblua5.4-0")
    endif()
    foreach(_dve_group runtime editor tools)
        string(TOUPPER "${_dve_group}" _dve_GROUP)
        set(CPACK_DEBIAN_${_dve_GROUP}_PACKAGE_DEPENDS "${_dve_hand_depends}")
    endforeach()
    message(STATUS "dpkg-shlibdeps not found: dve DEB packages use a hand-written Depends list")
endif()
# dve-dev: static archives, so shlibdeps has nothing to scan. Depend on the -dev packages that
# own the libraries and CMake packages the exported targets reference (found with dpkg -S).
list(JOIN DVE_DEV_DEBIAN_DEPENDS ", " CPACK_DEBIAN_DEV_PACKAGE_DEPENDS)

# Windows stubs (untested).
set(CPACK_NSIS_PACKAGE_NAME "DVE ${PROJECT_VERSION}")
set(CPACK_NSIS_DISPLAY_NAME "Destructible Voxel Engine ${PROJECT_VERSION}")
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\dve_player.exe")
set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")

# Per-generator adjustments (the DEB drops the bundled-library components).
set(CPACK_PROJECT_CONFIG_FILE "${PROJECT_BINARY_DIR}/DveCPackProjectConfig.cmake")
configure_file("${CMAKE_CURRENT_LIST_DIR}/DveCPackProjectConfig.cmake.in"
    "${CPACK_PROJECT_CONFIG_FILE}" @ONLY)

include(CPack)
