# libsndfile for dve_audio_synth (sample decoding: WAV/AIFF/FLAC/Ogg Vorbis/Ogg Opus and the other
# libsndfile formats; DVE only ever opens files for reading, see src/audio/sndfile_decoder.cpp).
#
# DVE_FETCH_SNDFILE=ON (default): build the pinned libsndfile 1.2.2 source (the upstream tag
# tarball Debian uses as its orig.tar.gz) with Debian 1.2.2-2+deb13u1's patches (security fixes,
# third_party/libsndfile/patches) as a SHARED library with ENABLE_MPEG=OFF, so neither libmpg123
# nor libmp3lame is linked or bundled. FLAC, Ogg, Vorbis and Opus come from the system (-dev
# packages) and are bundled next to it as before. The build runs once at configure time (cached
# in <build>/_deps/sndfile, keyed on the source hash, the patches and the options) so the choice
# below is known when the install rules and notices are generated.
#
# Why shared: libsndfile is LGPL-2.1-or-later. As a separate lib/dve/libsndfile.so.1 a user can
# replace it (LGPL-2.1 section 6), and we only have to provide its corresponding source. Linking
# it statically would oblige us to also ship the player's object files (or source) for relinking.
#
# Fallback (DVE_FETCH_SNDFILE=OFF, no network and no DVE_SNDFILE_ARCHIVE, a missing codec -dev
# package, or a failed build): the system libsndfile is linked, but it is treated as a system
# library, i.e. NOT bundled (games then need the distribution's libsndfile1 at run time). Either
# way no MP3 library ends up in lib/dve or a packaged game.
#
# Results:
#   DVE_SNDFILE_PROVIDER     fetched | system | none
#   DVE_SNDFILE_LINK         what dve_audio_synth links (imported target or library file)
#   DVE_SNDFILE_LIBRARY_DIR  folder of the fetched libsndfile.so* (fetched only)
#   DVE_SNDFILE_SOURCE_DIR   patched source tree (fetched only; license texts come from it)
#   DVE_SNDFILE_SOURCE_INFO  one-line description of the corresponding source (fetched only)
include_guard(GLOBAL)

option(DVE_FETCH_SNDFILE
    "Build the pinned libsndfile 1.2.2 (+ Debian security patches) as a shared library without MPEG/MP3 (no libmpg123/libmp3lame) and bundle it; OFF or on failure: link the system libsndfile without bundling it"
    ON)
set(DVE_SNDFILE_ARCHIVE "" CACHE FILEPATH
    "Local copy of the pinned libsndfile source tarball (libsndfile-1.2.2.tar.gz, offline builds)")

set(DVE_SNDFILE_VERSION 1.2.2)
set(DVE_SNDFILE_SHA256 ffe12ef8add3eaca876f04087734e6e8e029350082f3251f565fa9da55b52121)
# The upstream tag tarball is byte-identical to Debian's libsndfile_1.2.2.orig.tar.gz; the second
# URL is that file on snapshot.debian.org (by its SHA-1), which never disappears.
set(DVE_SNDFILE_URLS
    "https://github.com/libsndfile/libsndfile/archive/refs/tags/${DVE_SNDFILE_VERSION}.tar.gz"
    "https://snapshot.debian.org/file/28acc1c19b06c18f38f906d7efef404b2078a19a")
set(DVE_SNDFILE_PATCH_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/libsndfile/patches")
cmake_path(NORMAL_PATH DVE_SNDFILE_PATCH_DIR)
set(DVE_SNDFILE_DEBIAN_VERSION "1.2.2-2+deb13u1")
# Only what DVE needs: the library, no programs/examples/tests/packaging, no MPEG, and no
# optional extras that would add dependencies (ALSA is only used by sndfile-play; Speex and
# SQLite are experimental/regtest only).
set(DVE_SNDFILE_OPTIONS
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_SHARED_LIBS=ON
    -DENABLE_MPEG=OFF
    -DENABLE_EXTERNAL_LIBS=ON
    -DENABLE_EXPERIMENTAL=OFF
    -DBUILD_PROGRAMS=OFF
    -DBUILD_EXAMPLES=OFF
    -DBUILD_TESTING=OFF
    -DBUILD_REGTEST=OFF
    -DENABLE_CPACK=OFF
    -DENABLE_PACKAGE_CONFIG=OFF
    -DINSTALL_PKGCONFIG_MODULE=OFF
    -DINSTALL_MANPAGES=OFF
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_DISABLE_FIND_PACKAGE_mpg123=ON
    -DCMAKE_DISABLE_FIND_PACKAGE_mp3lame=ON
    -DCMAKE_DISABLE_FIND_PACKAGE_ALSA=ON
    -DCMAKE_DISABLE_FIND_PACKAGE_Speex=ON
    -DCMAKE_DISABLE_FIND_PACKAGE_SQLite3=ON)
# NEEDED entries the fetched library must (not) have.
set(DVE_SNDFILE_REQUIRED_CODECS "libFLAC\\.so" "libogg\\.so" "libvorbis\\.so" "libvorbisenc\\.so" "libopus\\.so")
set(DVE_SNDFILE_FORBIDDEN_LIBRARIES "libmpg123\\.so" "libmp3lame\\.so")

set(DVE_SNDFILE_PROVIDER none)
set(DVE_SNDFILE_LINK "")
set(DVE_SNDFILE_LIBRARY_DIR "")
set(DVE_SNDFILE_SOURCE_DIR "")
set(DVE_SNDFILE_SOURCE_INFO "")

function(_dve_sndfile_fetch out_ok)
    set(${out_ok} FALSE PARENT_SCOPE)
    set(root "${CMAKE_BINARY_DIR}/_deps/sndfile")
    set(source "${root}/src/libsndfile-${DVE_SNDFILE_VERSION}")
    set(build "${root}/build")
    set(prefix "${root}/install")
    set(library "${prefix}/lib/libsndfile.so.1")

    # The codec development files libsndfile's ENABLE_EXTERNAL_LIBS needs. Without them it would
    # silently build without FLAC/Ogg, which would be a regression, so fall back instead.
    set(missing "")
    foreach(pair "FLAC;FLAC/stream_decoder.h;libflac-dev" "ogg;ogg/ogg.h;libogg-dev"
                 "vorbis;vorbis/codec.h;libvorbis-dev" "vorbisenc;vorbis/vorbisenc.h;libvorbis-dev"
                 "opus;opus/opus.h;libopus-dev")
        list(GET pair 0 lib)
        list(GET pair 1 header)
        list(GET pair 2 package)
        find_library(DVE_SNDFILE_CODEC_${lib} NAMES ${lib})
        find_path(DVE_SNDFILE_CODEC_${lib}_INCLUDE NAMES ${header})
        if(NOT DVE_SNDFILE_CODEC_${lib} OR NOT DVE_SNDFILE_CODEC_${lib}_INCLUDE)
            list(APPEND missing ${package})
        endif()
    endforeach()
    if(missing)
        list(REMOVE_DUPLICATES missing)
        message(WARNING "DVE_FETCH_SNDFILE: codec development files missing (${missing}); "
            "falling back to the system libsndfile (not bundled)")
        return()
    endif()

    file(GLOB_RECURSE patches LIST_DIRECTORIES false "${DVE_SNDFILE_PATCH_DIR}/*")
    list(SORT patches)
    set(key "${DVE_SNDFILE_SHA256};${DVE_SNDFILE_OPTIONS};${CMAKE_GENERATOR};${CMAKE_C_COMPILER}")
    foreach(patch IN LISTS patches)
        file(SHA256 "${patch}" hash)
        string(APPEND key ";${hash}")
    endforeach()
    string(SHA256 key "${key}")
    set(stamp "${root}/dve-sndfile.stamp")
    if(EXISTS "${stamp}" AND EXISTS "${library}")
        file(READ "${stamp}" previous)
        if(previous STREQUAL key)
            set(DVE_SNDFILE_BUILT_LIBRARY "${library}" PARENT_SCOPE)
            set(DVE_SNDFILE_BUILT_SOURCE "${source}" PARENT_SCOPE)
            set(${out_ok} TRUE PARENT_SCOPE)
            return()
        endif()
    endif()
    file(REMOVE_RECURSE "${root}")
    file(MAKE_DIRECTORY "${root}")

    # Download (or take the local archive) and verify the pinned hash.
    set(archive "${root}/libsndfile-${DVE_SNDFILE_VERSION}.tar.gz")
    set(downloaded FALSE)
    if(DVE_SNDFILE_ARCHIVE)
        file(SHA256 "${DVE_SNDFILE_ARCHIVE}" hash)
        if(hash STREQUAL DVE_SNDFILE_SHA256)
            file(COPY_FILE "${DVE_SNDFILE_ARCHIVE}" "${archive}")
            set(downloaded TRUE)
        else()
            message(WARNING "DVE_SNDFILE_ARCHIVE ${DVE_SNDFILE_ARCHIVE} has SHA256 ${hash}, expected ${DVE_SNDFILE_SHA256}")
        endif()
    else()
        foreach(url IN LISTS DVE_SNDFILE_URLS)
            file(DOWNLOAD "${url}" "${archive}" EXPECTED_HASH SHA256=${DVE_SNDFILE_SHA256}
                TLS_VERIFY ON TIMEOUT 120 STATUS status)
            list(GET status 0 code)
            if(code EQUAL 0)
                set(downloaded TRUE)
                break()
            endif()
            list(GET status 1 text)
            message(STATUS "libsndfile: download from ${url} failed: ${text}")
        endforeach()
    endif()
    if(NOT downloaded)
        message(WARNING "DVE_FETCH_SNDFILE: could not get libsndfile ${DVE_SNDFILE_VERSION} "
            "(set DVE_SNDFILE_ARCHIVE to a local copy for offline builds); "
            "falling back to the system libsndfile (not bundled)")
        return()
    endif()
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${root}/src")

    # Debian's patch series, in order.
    find_program(DVE_PATCH_EXECUTABLE patch)
    find_program(DVE_GIT_EXECUTABLE git)
    file(STRINGS "${DVE_SNDFILE_PATCH_DIR}/series" series REGEX "^[^#]")
    foreach(entry IN LISTS series)
        string(STRIP "${entry}" entry)
        if(DVE_PATCH_EXECUTABLE)
            set(command "${DVE_PATCH_EXECUTABLE}" -p1 -N -s -i "${DVE_SNDFILE_PATCH_DIR}/${entry}")
        elseif(DVE_GIT_EXECUTABLE)
            set(command "${DVE_GIT_EXECUTABLE}" apply -p1 "${DVE_SNDFILE_PATCH_DIR}/${entry}")
        else()
            message(WARNING "DVE_FETCH_SNDFILE: neither patch nor git found to apply the security patches; "
                "falling back to the system libsndfile (not bundled)")
            return()
        endif()
        execute_process(COMMAND ${command} WORKING_DIRECTORY "${source}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
        if(NOT result EQUAL 0)
            message(WARNING "DVE_FETCH_SNDFILE: patch ${entry} failed:\n${output}\n"
                "falling back to the system libsndfile (not bundled)")
            return()
        endif()
    endforeach()

    message(STATUS "libsndfile: building ${DVE_SNDFILE_VERSION} (Debian ${DVE_SNDFILE_DEBIAN_VERSION} patches, "
        "shared, no MPEG) in ${root} ...")
    set(c_compiler "")
    if(CMAKE_C_COMPILER)
        set(c_compiler "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
    endif()
    set(log "${root}/build.log")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${source}" -B "${build}" -G "${CMAKE_GENERATOR}"
            ${c_compiler} "-DCMAKE_INSTALL_PREFIX=${prefix}" -Wno-dev ${DVE_SNDFILE_OPTIONS}
        RESULT_VARIABLE result OUTPUT_FILE "${root}/configure.log" ERROR_FILE "${root}/configure.log")
    if(result EQUAL 0)
        execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --parallel
            RESULT_VARIABLE result OUTPUT_FILE "${log}" ERROR_FILE "${log}")
    endif()
    if(result EQUAL 0)
        execute_process(COMMAND "${CMAKE_COMMAND}" --install "${build}" --strip
            RESULT_VARIABLE result OUTPUT_QUIET ERROR_FILE "${root}/install.log")
    endif()
    if(NOT result EQUAL 0 OR NOT EXISTS "${library}")
        message(WARNING "DVE_FETCH_SNDFILE: building libsndfile failed (see ${root}/*.log); "
            "falling back to the system libsndfile (not bundled)")
        return()
    endif()

    # The result must decode FLAC/Ogg/Vorbis/Opus and must not link any MPEG library.
    execute_process(COMMAND readelf -d "${library}" OUTPUT_VARIABLE dynamic RESULT_VARIABLE result)
    if(result EQUAL 0)
        foreach(pattern IN LISTS DVE_SNDFILE_REQUIRED_CODECS)
            if(NOT dynamic MATCHES "\\[${pattern}")
                message(WARNING "DVE_FETCH_SNDFILE: the built libsndfile does not link ${pattern}; "
                    "falling back to the system libsndfile (not bundled)")
                return()
            endif()
        endforeach()
        foreach(pattern IN LISTS DVE_SNDFILE_FORBIDDEN_LIBRARIES)
            if(dynamic MATCHES "\\[${pattern}")
                message(FATAL_ERROR "DVE_FETCH_SNDFILE: the built libsndfile links ${pattern} despite ENABLE_MPEG=OFF")
            endif()
        endforeach()
    endif()
    file(WRITE "${stamp}" "${key}")
    set(DVE_SNDFILE_BUILT_LIBRARY "${library}" PARENT_SCOPE)
    set(DVE_SNDFILE_BUILT_SOURCE "${source}" PARENT_SCOPE)
    set(${out_ok} TRUE PARENT_SCOPE)
endfunction()

if(DVE_FETCH_SNDFILE AND UNIX AND NOT APPLE)
    _dve_sndfile_fetch(_dve_sndfile_ok)
    if(_dve_sndfile_ok)
        add_library(dve_sndfile_imported SHARED IMPORTED GLOBAL)
        set_target_properties(dve_sndfile_imported PROPERTIES
            IMPORTED_LOCATION "${DVE_SNDFILE_BUILT_LIBRARY}"
            IMPORTED_SONAME libsndfile.so.1
            # Where dveConfig.cmake finds it in an installed prefix (bundled by the *Deps
            # components); when absent there, the consumer's find_library(libsndfile.so.1) picks
            # the system one.
            DVE_PACKAGE_FILE "${CMAKE_INSTALL_LIBDIR}/dve/libsndfile.so.1")
        set(DVE_SNDFILE_PROVIDER fetched)
        set(DVE_SNDFILE_LINK dve_sndfile_imported)
        get_filename_component(DVE_SNDFILE_LIBRARY_DIR "${DVE_SNDFILE_BUILT_LIBRARY}" DIRECTORY)
        set(DVE_SNDFILE_SOURCE_DIR "${DVE_SNDFILE_BUILT_SOURCE}")
        list(GET DVE_SNDFILE_URLS 0 _dve_sndfile_url)
        set(DVE_SNDFILE_SOURCE_INFO
            "libsndfile ${DVE_SNDFILE_VERSION} built from source by DVE: ${_dve_sndfile_url} (SHA256 ${DVE_SNDFILE_SHA256}, identical to Debian's libsndfile_1.2.2.orig.tar.gz) with the Debian ${DVE_SNDFILE_DEBIAN_VERSION} patches in third_party/libsndfile/patches of the DVE source, configured with CMake ${DVE_SNDFILE_OPTIONS}")
        list(JOIN DVE_SNDFILE_SOURCE_INFO " " DVE_SNDFILE_SOURCE_INFO)
    endif()
endif()

if(DVE_SNDFILE_PROVIDER STREQUAL "none")
    find_library(DVE_SNDFILE_LIBRARY NAMES sndfile libsndfile)
    if(NOT DVE_SNDFILE_LIBRARY AND UNIX)
        find_file(DVE_SNDFILE_RUNTIME_LIBRARY NAMES libsndfile.so.1 libsndfile.dylib
            PATHS /lib /usr/lib /usr/local/lib
            PATH_SUFFIXES x86_64-linux-gnu aarch64-linux-gnu)
        if(DVE_SNDFILE_RUNTIME_LIBRARY)
            set(DVE_SNDFILE_LIBRARY "${DVE_SNDFILE_RUNTIME_LIBRARY}")
        endif()
    endif()
    if(DVE_SNDFILE_LIBRARY)
        set(DVE_SNDFILE_PROVIDER system)
        set(DVE_SNDFILE_LINK "${DVE_SNDFILE_LIBRARY}")
    endif()
endif()
message(STATUS "libsndfile: ${DVE_SNDFILE_PROVIDER} ${DVE_SNDFILE_LINK}")
