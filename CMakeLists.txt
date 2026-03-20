cmake_minimum_required(VERSION 3.16)

# ── Derive PROJECT_VER from git tags ─────────────────────────────────────────
execute_process(
    COMMAND git describe --always --tags --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _git_result
)
if(NOT _git_result EQUAL 0 OR "${PROJECT_VER}" STREQUAL "")
    set(PROJECT_VER "0.0.0-dev")
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
