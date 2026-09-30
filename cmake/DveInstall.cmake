# install() rules, the exported `dve` CMake package and runtime-dependency bundling
# (packaging plan §4.1, §4.2). Included at the end of the top-level CMakeLists.txt, after every
# target exists. See docs/PACKAGING.md ("Installing and packaging").
#
# Components (CPack groups them into the dve-runtime/-editor/-tools/-dev packages):
#   Runtime       bin/dve_player
#   RuntimeDeps   lib/dve/*.so*  shared libraries dve_player needs that are not system libraries
#   Editor        bin/dve_desktop_editor, bin/dve_native_editor_x11, share/dve/assets
#   EditorDeps    lib/dve/*.so*  (the editor's non-system shared libraries)
#   Tools         bin/dve_pack, bin/dve_cook_*, bin/dve_asset_index, bin/dve_prefab_tool
#   ToolsDeps     lib/dve/*.so*
#   Development   include/dve/**, lib/libdve_*.a, lib/dve/third_party/*.a,
#                 lib/cmake/dve/{dveConfig,dveConfigVersion,dveTargets*}.cmake
# The *Deps components go into the archives (TGZ/ZIP) only; the DEB packages depend on the
# distribution's libraries instead (dpkg-shlibdeps), see cmake/DveCPack.cmake.
include_guard(GLOBAL)
include(CMakePackageConfigHelpers)

option(DVE_INSTALL "Generate install() rules and the exported dve CMake package" ON)
option(DVE_INSTALL_BUNDLE_RUNTIME_DEPENDENCIES
    "Install the non-system shared libraries of installed executables into lib/dve (Linux)" ON)
option(DVE_INSTALL_BUNDLE_JACK
    "Bundle libjack (and the Berkeley DB libdb it needs on JACK1) into lib/dve. Off: libjack must match the user's JACK server, and libdb's Sleepycat license reaches the software using it (see THIRD_PARTY_NOTICES)" OFF)
option(DVE_INSTALL_SAMPLE_MAPS
    "Install assets/audio/sample_maps with the editor assets (off until their provenance is documented, decision D8)" OFF)
if(NOT DVE_INSTALL)
    return()
endif()

set(DVE_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/dve")
set(DVE_INSTALL_BUNDLEDIR "${CMAKE_INSTALL_LIBDIR}/dve")
set(DVE_INSTALL_THIRD_PARTY_LIBDIR "${CMAKE_INSTALL_LIBDIR}/dve/third_party")
set(DVE_NOTICES_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/doc/dve")
set(DVE_SOURCE_TREE_DIR "${PROJECT_SOURCE_DIR}")
include(DveGamePackage)

# ----------------------------------------------------------------------------------------------
# Executables: RPATH and bundled runtime dependencies (§4.2)
# ----------------------------------------------------------------------------------------------
# Libraries every Linux desktop provides (and that must match the running system: libc, the
# display/audio/GPU stacks, dbus/udev/systemd, the font stack). Everything else an installed
# executable needs is copied into lib/dve. The names are matched before resolution, so the
# dependencies of excluded libraries are not walked either.
set(DVE_RUNTIME_DEPENDENCY_SYSTEM_EXCLUDES
    "^linux-vdso" "^ld-linux" "^libc\\.so" "^libm\\.so" "^libmvec\\.so" "^libdl\\.so"
    "^libpthread\\.so" "^librt\\.so" "^libutil\\.so" "^libresolv\\.so" "^libanl\\.so"
    "^libstdc\\+\\+\\.so" "^libgcc_s\\.so" "^libatomic\\.so"
    "^libX" "^libxcb" "^libxkbcommon" "^libwayland-" "^libdecor-" "^libGL" "^libEGL"
    "^libOpenGL" "^libGLX" "^libvulkan" "^libdrm\\.so" "^libgbm\\.so"
    "^libasound\\.so" "^libpulse" "^libpipewire-" "^libsndio\\.so"
    "^libdbus-1\\.so" "^libudev\\.so" "^libsystemd\\.so" "^libcap\\.so" "^libz\\.so"
    "^libbsd\\.so" "^libmd\\.so" "^libffi\\.so" "^libexpat\\.so" "^libasyncns\\.so"
    "^libglib-2" "^libgobject-2" "^libgio-2" "^libgmodule-2" "^libpcre2-"
    "^libselinux\\.so" "^libmount\\.so" "^libblkid\\.so" "^libuuid\\.so"
    "^liblzma\\.so" "^libzstd\\.so" "^liblz4\\.so" "^libgcrypt\\.so" "^libgpg-error\\.so"
    "^libfreetype\\.so" "^libharfbuzz" "^libfontconfig\\.so" "^libgraphite2\\.so"
    "^libbrotli" "^libbz2\\.so" "^libpng16\\.so"
    # Windows system DLLs, for when the Windows install rules are exercised.
    "^api-ms-" "^ext-ms-" "^(kernel|user|gdi|shell|ole|oleaut|advapi|comdlg|ws2_|winmm|imm|version|setupapi|dwmapi|uxtheme|dinput|dxgi|d3d|opengl|hid|cfgmgr)[0-9]*\\.dll$")
if(NOT DVE_INSTALL_BUNDLE_JACK)
    # Left to the system (libjack0 or libjack-jackd2-0); see the option above.
    list(APPEND DVE_RUNTIME_DEPENDENCY_SYSTEM_EXCLUDES "^libjack\\.so" "^libjackserver\\.so" "^libdb-[0-9]")
endif()
if(DVE_SNDFILE_PROVIDER STREQUAL "system")
    # Fallback of cmake/DveSndFile.cmake: the distribution's libsndfile links MP3 support
    # (libmpg123, libmp3lame), so it and its codecs are left to the system instead of bundled.
    list(APPEND DVE_RUNTIME_DEPENDENCY_SYSTEM_EXCLUDES "^libsndfile\\.so")
endif()
set(DVE_RUNTIME_DEPENDENCY_POST_EXCLUDES ".*[/\\\\][Ss]ystem32[/\\\\].*")

set(DVE_INSTALLED_EXECUTABLES "")
function(dve_install_program target component)
    if(NOT TARGET ${target})
        return()
    endif()
    if(APPLE)
        set_target_properties(${target} PROPERTIES INSTALL_RPATH "@loader_path/../${DVE_INSTALL_BUNDLEDIR}")
    elseif(UNIX)
        # DT_RPATH (not DT_RUNPATH): the loader applies it to every library in the process, so
        # a bundled library's own dependencies (e.g. libsndfile -> libFLAC) also resolve from
        # lib/dve without patchelf. DT_RUNPATH would only cover the executable's direct needs.
        # The Runtime program also looks in $ORIGIN/lib/dve: that is the layout of a shipped
        # game folder (dve_package_game: <Game>/<Game> + <Game>/lib/dve/*.so).
        set(_dve_rpath "$ORIGIN/../${DVE_INSTALL_BUNDLEDIR}")
        if(component STREQUAL "Runtime")
            list(APPEND _dve_rpath "$ORIGIN/${DVE_INSTALL_BUNDLEDIR}")
        endif()
        set_target_properties(${target} PROPERTIES INSTALL_RPATH "${_dve_rpath}")
        target_link_options(${target} PRIVATE "LINKER:--disable-new-dtags")
    endif()
    set(_dve_depset "")
    if(DVE_INSTALL_BUNDLE_RUNTIME_DEPENDENCIES AND NOT WIN32)
        set(_dve_depset RUNTIME_DEPENDENCY_SET dve_${component}_runtime_deps)
        set_property(GLOBAL APPEND PROPERTY DVE_RUNTIME_DEPENDENCY_SETS ${component})
    endif()
    install(TARGETS ${target} ${_dve_depset}
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ${component})
    if(WIN32 AND DVE_INSTALL_BUNDLE_RUNTIME_DEPENDENCIES)
        # Untested stub (no Windows runner yet): copy the DLLs CMake knows about next to the exe.
        install(FILES $<TARGET_RUNTIME_DLLS:${target}> DESTINATION "${CMAKE_INSTALL_BINDIR}"
            COMPONENT ${component}Deps OPTIONAL)
    endif()
    set_property(GLOBAL APPEND PROPERTY DVE_INSTALLED_EXECUTABLES ${target})
    set_property(GLOBAL APPEND PROPERTY DVE_INSTALL_PROGRAM_COMPONENTS ${component})
    set_property(GLOBAL APPEND PROPERTY DVE_INSTALL_PROGRAMS_${component} ${target})
endfunction()

# Runtime: the game player (never links dve_editor, see dve_player_no_editor_link).
dve_install_program(dve_player Runtime)

# Editor.
dve_install_program(dve_desktop_editor Editor)
dve_install_program(dve_native_editor_x11 Editor)
if(TARGET dve_desktop_editor OR TARGET dve_native_editor_x11)
    set(_dve_asset_excludes REGEX "/\\.autosave(/|$)" EXCLUDE)
    if(NOT DVE_INSTALL_SAMPLE_MAPS)
        # D8: sample-map provenance is undocumented, so never package them by default.
        list(APPEND _dve_asset_excludes REGEX "/audio/sample_maps(/|$)" EXCLUDE)
    endif()
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/assets/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/dve/assets"
        COMPONENT Editor
        ${_dve_asset_excludes})
endif()

# Tools: the cookers, dve_pack, dve_export_scene (editor scene -> DVOXSCENE) and the
# dve_package_game script (shippable game folder, Phase 4).
foreach(_dve_tool
        dve_pack dve_export_scene dve_cook_model dve_cook_mesh dve_cook_text3d dve_cook_gabor dve_cook_fluoddity
        dve_cook_hair dve_cook_audio dve_cook_chiptune dve_cook_sample_map dve_cook_heightmap
        dve_cook_sprite dve_asset_index dve_prefab_tool)
    dve_install_program(${_dve_tool} Tools)
endforeach()

if(TARGET dve_pack)
    install(PROGRAMS "${PROJECT_SOURCE_DIR}/scripts/dve_package_game.py"
        DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME dve_package_game COMPONENT Tools)
endif()

get_property(_dve_depset_components GLOBAL PROPERTY DVE_RUNTIME_DEPENDENCY_SETS)
list(REMOVE_DUPLICATES _dve_depset_components)
set(_dve_depset_directories "")
if(DVE_SNDFILE_LIBRARY_DIR)
    # The pinned no-MPEG libsndfile (cmake/DveSndFile.cmake); the executables' build RPATH
    # already points there, this also covers libraries that need it.
    list(APPEND _dve_depset_directories DIRECTORIES "${DVE_SNDFILE_LIBRARY_DIR}")
endif()
foreach(_dve_component IN LISTS _dve_depset_components)
    install(RUNTIME_DEPENDENCY_SET dve_${_dve_component}_runtime_deps
        PRE_EXCLUDE_REGEXES ${DVE_RUNTIME_DEPENDENCY_SYSTEM_EXCLUDES}
        POST_EXCLUDE_REGEXES ${DVE_RUNTIME_DEPENDENCY_POST_EXCLUDES}
        ${_dve_depset_directories}
        LIBRARY DESTINATION "${DVE_INSTALL_BUNDLEDIR}" COMPONENT ${_dve_component}Deps
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ${_dve_component}Deps)
endforeach()

# ----------------------------------------------------------------------------------------------
# Development: headers, static libraries and the exported package (§4.1)
# ----------------------------------------------------------------------------------------------
# Every public DVE library a game or tool built outside this tree may need. Libraries that are
# not built with the current options are skipped; dve_editor/dve_asset_pipeline stay private
# (the player must never need them).
set(DVE_EXPORTED_LIBRARIES "")
foreach(_dve_library
        dve_core dve_platform dve_platform_sdl3 dve_rhi dve_render_bridge dve_audio_synth
        dve_audio_sdl3 dve_player_runtime)
    if(TARGET ${_dve_library})
        list(APPEND DVE_EXPORTED_LIBRARIES ${_dve_library})
    endif()
endforeach()

# Walk the link interface of the exported libraries and sort every dependency into:
#   - DVE libraries (exported as dve::<name>),
#   - third-party targets built in this tree (vendored manifold; fetched Jolt/SDL/RtMidi/Box2D/
#     Box3D), which are exported alongside ours as dve::third_party_<name> with their archives in
#     lib/dve/third_party, so a DVE_FETCH_* build yields a self-contained package;
#   - namespaced imported targets from installed packages (SDL3::SDL3, Jolt::Jolt, ...), which
#     dveConfig.cmake re-finds with find_dependency();
#   - local imported helper targets (the Lua ABI fallback `dve_lua54_runtime`, `PkgConfig::DVE_LUA`,
#     `dve_rtmidi_imported`, legacy Jolt/Box pairs), which dveConfig.cmake recreates from the
#     recorded library files;
#   - plain library files (absolute paths such as libsndfile), kept as-is.
set(_dve_pending ${DVE_EXPORTED_LIBRARIES})
set(_dve_seen "")
set(DVE_EXPORTED_THIRD_PARTY "")
set(_dve_packages "")
set(_dve_recreated "")
set(_dve_library_files "")
set(DVE_INSTALL_EXPORT_PROBLEMS "")
while(_dve_pending)
    list(POP_FRONT _dve_pending _dve_target)
    if(_dve_target IN_LIST _dve_seen)
        continue()
    endif()
    list(APPEND _dve_seen ${_dve_target})
    get_target_property(_dve_links ${_dve_target} INTERFACE_LINK_LIBRARIES)
    if(NOT _dve_links)
        continue()
    endif()
    foreach(_dve_item IN LISTS _dve_links)
        string(REGEX MATCHALL "[A-Za-z0-9_.+/-]+(::[A-Za-z0-9_.+-]+)*" _dve_tokens "${_dve_item}")
        foreach(_dve_token IN LISTS _dve_tokens)
            if(_dve_token MATCHES "^/" AND EXISTS "${_dve_token}" AND NOT IS_DIRECTORY "${_dve_token}")
                list(APPEND _dve_library_files "${_dve_token}")
                continue()
            endif()
            if(NOT TARGET "${_dve_token}")
                continue()
            endif()
            set(_dve_dep "${_dve_token}")
            get_target_property(_dve_aliased "${_dve_dep}" ALIASED_TARGET)
            if(_dve_aliased)
                set(_dve_dep "${_dve_aliased}")
            endif()
            get_target_property(_dve_imported "${_dve_dep}" IMPORTED)
            if(_dve_imported)
                if(_dve_dep MATCHES "^([A-Za-z0-9_]+)::" AND NOT CMAKE_MATCH_1 STREQUAL "PkgConfig")
                    list(APPEND _dve_packages "${CMAKE_MATCH_1}")
                else()
                    list(APPEND _dve_recreated "${_dve_dep}")
                endif()
            elseif(_dve_dep MATCHES "^dve_")
                if(NOT _dve_dep IN_LIST DVE_EXPORTED_LIBRARIES)
                    list(APPEND DVE_EXPORTED_LIBRARIES "${_dve_dep}")
                endif()
                list(APPEND _dve_pending "${_dve_dep}")
            else()
                if(NOT _dve_dep IN_LIST DVE_EXPORTED_THIRD_PARTY)
                    list(APPEND DVE_EXPORTED_THIRD_PARTY "${_dve_dep}")
                endif()
                list(APPEND _dve_pending "${_dve_dep}")
            endif()
        endforeach()
    endforeach()
endwhile()
list(REMOVE_DUPLICATES _dve_packages)
list(REMOVE_DUPLICATES _dve_recreated)
list(REMOVE_DUPLICATES _dve_library_files)

# dveConfig.cmake snippets.
set(DVE_CONFIG_FIND_DEPENDENCIES "")
foreach(_dve_package IN LISTS _dve_packages)
    set(_dve_hint "")
    if(DEFINED ${_dve_package}_DIR AND ${_dve_package}_DIR AND NOT _dve_package STREQUAL "Threads")
        set(_dve_hint " HINTS \"${${_dve_package}_DIR}\"")
    endif()
    string(APPEND DVE_CONFIG_FIND_DEPENDENCIES "find_dependency(${_dve_package}${_dve_hint})\n")
endforeach()

set(DVE_CONFIG_RECREATED_TARGETS "")
foreach(_dve_dep IN LISTS _dve_recreated)
    get_target_property(_dve_type "${_dve_dep}" TYPE)
    string(REPLACE "_LIBRARY" "" _dve_kind "${_dve_type}")
    if(_dve_kind STREQUAL "MODULE")
        set(_dve_kind SHARED)
    endif()
    set(_dve_location "")
    foreach(_dve_property IMPORTED_LOCATION IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_NOCONFIG
                          IMPORTED_LOCATION_NONE IMPORTED_LOCATION_RELWITHDEBINFO)
        if(NOT _dve_location AND NOT _dve_kind STREQUAL "INTERFACE")
            get_target_property(_dve_location "${_dve_dep}" ${_dve_property})
            if(NOT _dve_location)
                set(_dve_location "")
            endif()
        endif()
    endforeach()
    get_target_property(_dve_dep_links "${_dve_dep}" INTERFACE_LINK_LIBRARIES)
    if(NOT _dve_dep_links)
        set(_dve_dep_links "")
    endif()
    # A library built in this tree and bundled into lib/dve (the pinned libsndfile) is found
    # relative to the installed package; the build-tree path must not leak into it.
    get_target_property(_dve_package_file "${_dve_dep}" DVE_PACKAGE_FILE)
    if(_dve_package_file)
        set(_dve_location "\${PACKAGE_PREFIX_DIR}/${_dve_package_file}")
    endif()
    foreach(_dve_file ${_dve_location} ${_dve_dep_links})
        if(_dve_file MATCHES "^/" AND EXISTS "${_dve_file}")
            list(APPEND _dve_library_files "${_dve_file}")
        endif()
    endforeach()
    string(APPEND DVE_CONFIG_RECREATED_TARGETS
        "_dve_recreate_imported(\"${_dve_dep}\" ${_dve_kind} \"${_dve_location}\" \"${_dve_dep_links}\")\n")
endforeach()
list(REMOVE_DUPLICATES _dve_library_files)

# Third-party targets built here: exported with our targets. Record problems instead of
# failing the build; the Development component and the package are then skipped.
foreach(_dve_dep IN LISTS DVE_EXPORTED_THIRD_PARTY)
    get_target_property(_dve_type "${_dve_dep}" TYPE)
    if(_dve_type STREQUAL "SHARED_LIBRARY" OR _dve_type STREQUAL "MODULE_LIBRARY")
        list(APPEND DVE_INSTALL_EXPORT_PROBLEMS
            "${_dve_dep} is a shared library built in this tree; build it statically (DVE targets are static)")
    endif()
    string(MAKE_C_IDENTIFIER "${_dve_dep}" _dve_export_name)
    set_target_properties("${_dve_dep}" PROPERTIES EXPORT_NAME "third_party_${_dve_export_name}")
endforeach()
foreach(_dve_library IN LISTS DVE_EXPORTED_LIBRARIES)
    get_target_property(_dve_type "${_dve_library}" TYPE)
    if(NOT _dve_type STREQUAL "STATIC_LIBRARY")
        list(APPEND DVE_INSTALL_EXPORT_PROBLEMS "${_dve_library} is ${_dve_type}; the dve package expects static libraries")
    endif()
    string(REGEX REPLACE "^dve_" "" _dve_export_name "${_dve_library}")
    set_target_properties("${_dve_library}" PROPERTIES EXPORT_NAME "${_dve_export_name}")
endforeach()

# Build options that change public headers or class layouts. They travel to consumers as the
# PUBLIC compile definitions of dve::core (INTERFACE_COMPILE_DEFINITIONS in dveTargets.cmake);
# dve/build_config.hpp records them so a translation unit can verify it sees the same set.
set(DVE_LAYOUT_DEFINES
    DVE_ENABLE_FLUODDITY DVE_ENABLE_SOFT_BODIES DVE_ENABLE_CPU_HAIR DVE_ENABLE_VFX_PARTICLES
    DVE_ENABLE_PBF_LIQUID DVE_ENABLE_SIMULATION_ENERGY_COMPILER DVE_ENABLE_BSPLINE_CLOTH
    DVE_ENABLE_DEFORMABLE_RUNTIME DVE_ENABLE_GRID_FLUIDS DVE_ENABLE_FLIP_LIQUIDS
    DVE_GEOMETRY_MODE_VOXEL DVE_GEOMETRY_MODE_POLYGON DVE_GEOMETRY_MODE_HYBRID
    DVE_HAVE_BOX2D DVE_HAVE_BOX3D DVE_HAVE_JOLT DVE_HAVE_MANIFOLD DVE_HAVE_LUA)
get_target_property(_dve_core_defines dve_core INTERFACE_COMPILE_DEFINITIONS)
if(NOT _dve_core_defines)
    set(_dve_core_defines "")
endif()
set(DVE_BUILD_CONFIG_BODY "")
set(DVE_BUILD_CONFIG_DEFINE_LIST "")
foreach(_dve_define IN LISTS DVE_LAYOUT_DEFINES)
    set(_dve_on OFF)
    foreach(_dve_entry IN LISTS _dve_core_defines)
        if(_dve_entry MATCHES "^${_dve_define}(=.*)?$")
            set(_dve_on ON)
        endif()
    endforeach()
    if(_dve_on)
        list(APPEND DVE_BUILD_CONFIG_DEFINE_LIST ${_dve_define})
        string(APPEND DVE_BUILD_CONFIG_BODY
            "#define DVE_BUILD_CONFIG_${_dve_define} 1\n"
            "#if !defined(DVE_BUILD_CONFIG_NO_CHECK) && !defined(${_dve_define})\n"
            "#error \"dve was built with ${_dve_define}; compile with the dve::core interface definitions (link dve::core)\"\n"
            "#endif\n")
    else()
        string(APPEND DVE_BUILD_CONFIG_BODY
            "#define DVE_BUILD_CONFIG_${_dve_define} 0\n"
            "#if !defined(DVE_BUILD_CONFIG_NO_CHECK) && defined(${_dve_define})\n"
            "#error \"dve was built without ${_dve_define}, but it is defined in this translation unit\"\n"
            "#endif\n")
    endif()
endforeach()
list(JOIN DVE_BUILD_CONFIG_DEFINE_LIST ";" DVE_BUILD_CONFIG_DEFINE_STRING)
configure_file("${CMAKE_CURRENT_LIST_DIR}/build_config.hpp.in"
    "${DVE_GENERATED_INCLUDE_DIR}/dve/build_config.hpp" @ONLY)

# Options recorded in dveConfig.cmake (informational; the definitions come with the targets).
set(DVE_CONFIG_HAVE_LUA OFF)
set(DVE_CONFIG_HAVE_JOLT OFF)
if("DVE_HAVE_LUA" IN_LIST DVE_BUILD_CONFIG_DEFINE_LIST)
    set(DVE_CONFIG_HAVE_LUA ON)
endif()
if("DVE_HAVE_JOLT" IN_LIST DVE_BUILD_CONFIG_DEFINE_LIST)
    set(DVE_CONFIG_HAVE_JOLT ON)
endif()
set(DVE_CONFIG_SDL3_BUNDLED OFF)
if(DVE_SDL3_FETCHED)
    set(DVE_CONFIG_SDL3_BUNDLED ON)
endif()
list(JOIN DVE_EXPORTED_LIBRARIES ";" DVE_CONFIG_EXPORTED_LIBRARIES)
list(TRANSFORM DVE_CONFIG_EXPORTED_LIBRARIES REPLACE "dve_" "")

set(DVE_INSTALL_DEVELOPMENT ON)
if(DVE_INSTALL_EXPORT_PROBLEMS)
    set(DVE_INSTALL_DEVELOPMENT OFF)
    list(JOIN DVE_INSTALL_EXPORT_PROBLEMS "\n  " _dve_problem_text)
    message(WARNING "dve CMake package disabled (Development component not installed):\n  ${_dve_problem_text}")
endif()

# Debian packages that provide the recorded library files and packages, for dve-dev's Depends
# (dpkg-shlibdeps only looks at ELF executables and shared libraries, not static archives).
set(DVE_DEV_DEBIAN_DEPENDS "")
find_program(DVE_DPKG_QUERY dpkg-query)
find_program(DVE_DPKG dpkg)
if(DVE_SNDFILE_PROVIDER STREQUAL "fetched")
    # Without the bundled copy, a dve-dev consumer links the distribution's libsndfile.so.1.
    find_file(DVE_SYSTEM_SNDFILE_RUNTIME NAMES libsndfile.so.1
        PATHS /lib /usr/lib /usr/local/lib PATH_SUFFIXES x86_64-linux-gnu aarch64-linux-gnu NO_CACHE)
    if(DVE_SYSTEM_SNDFILE_RUNTIME)
        list(APPEND _dve_library_files "${DVE_SYSTEM_SNDFILE_RUNTIME}")
    endif()
endif()
if(DVE_DPKG)
    set(_dve_owned_files ${_dve_library_files})
    foreach(_dve_package IN LISTS _dve_packages)
        if(DEFINED ${_dve_package}_DIR AND EXISTS "${${_dve_package}_DIR}")
            file(GLOB _dve_config_files "${${_dve_package}_DIR}/*onfig.cmake")
            list(APPEND _dve_owned_files ${_dve_config_files})
        endif()
    endforeach()
    foreach(_dve_file IN LISTS _dve_owned_files)
        execute_process(COMMAND "${DVE_DPKG}" -S "${_dve_file}"
            OUTPUT_VARIABLE _dve_owner ERROR_QUIET RESULT_VARIABLE _dve_result
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_dve_result EQUAL 0 AND _dve_owner MATCHES "^([a-z0-9][a-z0-9+.-]+)(:[a-z0-9-]+)?: ")
            list(APPEND DVE_DEV_DEBIAN_DEPENDS "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES DVE_DEV_DEBIAN_DEPENDS)
endif()

if(DVE_INSTALL_DEVELOPMENT)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/dve"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT Development
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h" PATTERN "*.inl")
    install(FILES
            "${DVE_GENERATED_INCLUDE_DIR}/dve/version.hpp"
            "${DVE_GENERATED_INCLUDE_DIR}/dve/build_config.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/dve"
        COMPONENT Development)
    install(TARGETS ${DVE_EXPORTED_LIBRARIES}
        EXPORT dveTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    if(DVE_EXPORTED_THIRD_PARTY)
        install(TARGETS ${DVE_EXPORTED_THIRD_PARTY}
            EXPORT dveTargets
            ARCHIVE DESTINATION "${DVE_INSTALL_THIRD_PARTY_LIBDIR}" COMPONENT Development
            LIBRARY DESTINATION "${DVE_INSTALL_THIRD_PARTY_LIBDIR}" COMPONENT Development
            FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/dve/third_party" COMPONENT Development)
    endif()
    install(EXPORT dveTargets
        NAMESPACE dve::
        DESTINATION "${DVE_INSTALL_CMAKEDIR}"
        COMPONENT Development)
    configure_package_config_file("${CMAKE_CURRENT_LIST_DIR}/dveConfig.cmake.in"
        "${PROJECT_BINARY_DIR}/dveConfig.cmake"
        INSTALL_DESTINATION "${DVE_INSTALL_CMAKEDIR}")
    write_basic_package_version_file("${PROJECT_BINARY_DIR}/dveConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMinorVersion)
    install(FILES "${PROJECT_BINARY_DIR}/dveConfig.cmake" "${PROJECT_BINARY_DIR}/dveConfigVersion.cmake"
            "${CMAKE_CURRENT_LIST_DIR}/DveGamePackage.cmake"
        DESTINATION "${DVE_INSTALL_CMAKEDIR}"
        COMPONENT Development)
endif()

list(JOIN DVE_EXPORTED_LIBRARIES " " _dve_msg_libs)
list(JOIN DVE_EXPORTED_THIRD_PARTY " " _dve_msg_third)
list(JOIN _dve_packages " " _dve_msg_packages)
list(JOIN _dve_recreated " " _dve_msg_recreated)
message(STATUS "dve install: exported [${_dve_msg_libs}] third-party [${_dve_msg_third}] "
               "packages [${_dve_msg_packages}] recreated [${_dve_msg_recreated}]")

get_property(DVE_INSTALLED_EXECUTABLES GLOBAL PROPERTY DVE_INSTALLED_EXECUTABLES)
get_property(_dve_program_components GLOBAL PROPERTY DVE_INSTALL_PROGRAM_COMPONENTS)
set(DVE_INSTALLED_COMPONENTS "")
foreach(_dve_component Runtime Editor Tools)
    if(_dve_component IN_LIST _dve_program_components)
        list(APPEND DVE_INSTALLED_COMPONENTS ${_dve_component})
        if(_dve_component IN_LIST _dve_depset_components)
            list(APPEND DVE_INSTALLED_COMPONENTS ${_dve_component}Deps)
        endif()
    endif()
endforeach()
if(DVE_INSTALL_DEVELOPMENT)
    list(APPEND DVE_INSTALLED_COMPONENTS Development)
endif()
message(STATUS "dve install components: ${DVE_INSTALLED_COMPONENTS}")

include(DveLicense)
include(DveNotices)
include(DveInstallTests)
