cmake_minimum_required(VERSION 3.16)

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
