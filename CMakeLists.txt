cmake_minimum_required(VERSION 3.16)

# ── Derive PROJECT_VER from git tags ─────────────────────────────────────────
# The derivation is scripts/project_ver.py rather than inline git calls because
# it is load-bearing and was untestable from here: it decides whether a release
# binary self-reports "-dirty", and main/CMakeLists.txt feeds the result to
# FW_VERSION and to the embedded web UI, where cmpVer() compares it against the
# published manifest. test/project_ver/ drives that same script over throwaway
# repos; see its header for the rules and the failure each one prevents.
#
# RELEASE_TAG is the tag firmware-release.yml is building, empty everywhere else;
# the script falls back to git describe when it is. It is handed to the script
# rather than set straight into PROJECT_VER so the dirty checks still run on the
# release path. Both reasons are spelled out in project_ver.py's main().
set(RELEASE_TAG "" CACHE STRING "Tag being released; set by firmware-release.yml")
execute_process(
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/scripts/project_ver.py
            --repo ${CMAKE_CURRENT_LIST_DIR}
            # Quoted: a tag name may legally contain a semicolon, which CMake
            # would otherwise read as a list separator and split into a second,
            # unrecognised argument.
            "--release-tag=${RELEASE_TAG}"
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE _ver_err
    ERROR_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _ver_result
)
# STREQUAL "0", not EQUAL 0: CMake puts a human-readable error string in
# RESULT_VARIABLE when the command cannot be launched at all, and
# if(NOT "no such file or directory" EQUAL 0) is true — which is how the
# previous inline check turned a broken toolchain into a plausible-looking
# "-dirty" release. The script owns every legitimate fallback, so anything
# reaching here is a broken toolchain; fail loudly rather than mislabel a
# binary, which is how three releases shipped "-dirty" unnoticed.
if(NOT "${_ver_result}" STREQUAL "0" OR "${PROJECT_VER}" STREQUAL "")
    message(FATAL_ERROR "project_ver.py failed (${_ver_result}): ${_ver_err}")
endif()
# The script reports a tag tie, and its reason for giving up on establishing
# cleanliness, on stderr while still exiting 0. Captured stderr that is only
# read in the fatal branch above would throw both away — and a build that
# silently labels itself "-dirty" with no reason in the log is how this went
# unnoticed for three releases.
if(NOT "${_ver_err}" STREQUAL "")
    message(WARNING "${_ver_err}")
endif()

# esp-homekit-sdk components (submodule)
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/components/esp-homekit-sdk/components/homekit/esp_hap_core
    ${CMAKE_CURRENT_LIST_DIR}/components/esp-homekit-sdk/components/homekit/esp_hap_apple_profiles
    ${CMAKE_CURRENT_LIST_DIR}/components/esp-homekit-sdk/components/homekit/esp_hap_extras
    ${CMAKE_CURRENT_LIST_DIR}/components/esp-homekit-sdk/components/homekit/esp_hap_platform
    ${CMAKE_CURRENT_LIST_DIR}/components/esp-homekit-sdk/components/homekit/hkdf-sha
    ${CMAKE_CURRENT_LIST_DIR}/components/esp-homekit-sdk/components/homekit/mu_srp
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mitsubishi-cn105-homekit)
