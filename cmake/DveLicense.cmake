# The engine's own license (MIT, the root LICENSE) in every package.
#
# Each package group (dve-runtime, dve-editor, dve-tools, dve-dev) installs it to
# share/doc/dve-<group>/ from its main component (Runtime, Editor, Tools, Development):
#   LICENSE    the root LICENSE, verbatim
#   copyright  the same text as a Debian machine-readable copyright file (DEP-5), which is
#              where Debian policy expects it in the .deb (/usr/share/doc/<package>/copyright)
# One folder per package keeps the .debs from owning the same file. The hidden *Deps components
# always ship in the same archive as their main component, so they need no copy of their own.
# The THIRD_PARTY_NOTICES files (share/doc/dve) repeat the engine license at their top.
include_guard(GLOBAL)
set(DVE_LICENSE_FILE "${PROJECT_SOURCE_DIR}/LICENSE")
set(DVE_COPYRIGHT_HOLDER "Benjamin Schulz")
set(DVE_COPYRIGHT_YEARS "2026")
if(NOT EXISTS "${DVE_LICENSE_FILE}")
    message(FATAL_ERROR "dve: ${DVE_LICENSE_FILE} is missing")
endif()

file(READ "${DVE_LICENSE_FILE}" _dve_license_text)
string(REGEX REPLACE "\n+$" "" _dve_license_text "${_dve_license_text}")
# DEP-5 license text: every line indented by one space, empty lines written as " .".
string(REPLACE "\n" ";" _dve_license_lines "${_dve_license_text}")
set(_dve_license_body "")
foreach(_dve_line IN LISTS _dve_license_lines)
    if(_dve_line STREQUAL "")
        string(APPEND _dve_license_body " .\n")
    else()
        string(APPEND _dve_license_body " ${_dve_line}\n")
    endif()
endforeach()

set(DVE_LICENSE_PACKAGES "")
foreach(_dve_component Runtime Editor Tools Development)
    if(NOT _dve_component IN_LIST DVE_INSTALLED_COMPONENTS)
        continue()
    endif()
    string(TOLOWER "${_dve_component}" _dve_group)
    if(_dve_group STREQUAL "development")
        set(_dve_group dev)
    endif()
    set(_dve_copyright "${PROJECT_BINARY_DIR}/license/dve-${_dve_group}/copyright")
    file(CONFIGURE OUTPUT "${_dve_copyright}" CONTENT
"Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: dve
Upstream-Contact: HiroSakuraba <52479046+HiroSakuraba@users.noreply.github.com>
Source: https://github.com/HiroSakuraba/destructible-voxel-engine
Comment: Third-party software in this package, and the licenses that apply to it, are listed
 in share/doc/dve/THIRD_PARTY_NOTICES-${_dve_group}.txt (/usr/share/doc/dve/ in the .deb).

Files: *
Copyright: ${DVE_COPYRIGHT_YEARS} ${DVE_COPYRIGHT_HOLDER}
License: Expat

License: Expat
${_dve_license_body}" @ONLY)
    install(FILES "${DVE_LICENSE_FILE}" "${_dve_copyright}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/dve-${_dve_group}"
        COMPONENT ${_dve_component})
    list(APPEND DVE_LICENSE_PACKAGES "dve-${_dve_group}")
endforeach()
message(STATUS "dve license: MIT, installed for [${DVE_LICENSE_PACKAGES}]")
