cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
# A build that embeds this package owns its cache, its try_compile settings and
# the SDK (the SDK's docs/package_contract.md, "Embedded build").
if(NOT LOGONOMIC_CLI_ADDON_EMBEDDED)
    set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Do not capture the configure environment" FORCE)
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
endif()
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
elseif(CMAKE_GENERATOR STREQUAL "FASTBuild")
    message(FATAL_ERROR "FASTBuild package builds require direct FetchContent population (CMP0168).")
endif()

project(logonomic_cli_opencode VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    if(NOT TARGET logonomic_cli_addon_sdk::sdk)
        message(FATAL_ERROR "The embedding build must add the Logonomic CLI add-on SDK before this package.")
    endif()
else()
    set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
    set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
        add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/sdk")
    else()
        include(FetchContent)
        FetchContent_Declare(logonomic_sdk_source
            GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
        FetchContent_MakeAvailable(logonomic_sdk_source)
    endif()
endif()

function(opencode_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
endfunction()

add_library(opencode_provider STATIC
    src/opencode_acp.cpp
    src/opencode_launcher.cpp
    src/opencode_lifecycle.cpp
    src/opencode_preparation.cpp)
opencode_target(opencode_provider)
target_include_directories(opencode_provider PUBLIC src)
target_link_libraries(opencode_provider PUBLIC logonomic_cli_addon_sdk::sdk)

add_executable(opencode_adapter src/adapter.cpp)
target_link_libraries(opencode_adapter PRIVATE opencode_provider)
opencode_target(opencode_adapter)

add_executable(opencode_helper src/opencode_helper.cpp)
opencode_target(opencode_helper)
logonomic_cli_addon_add_launch_bridge(opencode_launch_bridge)
opencode_target(opencode_launch_bridge)

logonomic_cli_addon_package_platform(OPENCODE_PLATFORM)
set(OPENCODE_EXECUTABLE_SUFFIX "${CMAKE_EXECUTABLE_SUFFIX}")
logonomic_cli_addon_package_directory(logonomic.cli.opencode OPENCODE_PACKAGE_DIRECTORY)
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
file(GLOB_RECURSE package_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/package/assets/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/notices/*")
# Directory membership is an input too: removing a resource, or adding one
# with an older timestamp, must rebuild the indexed package. file(GENERATE)
# changes this file only when membership changes, preserving no-op builds.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/package-inputs.txt"
    CONTENT "${package_files}\n")
add_custom_command(
    OUTPUT "${OPENCODE_PACKAGE_DIRECTORY}/package.files.json"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${OPENCODE_PACKAGE_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${OPENCODE_PACKAGE_DIRECTORY}/adapter" "${OPENCODE_PACKAGE_DIRECTORY}/helpers"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/package/assets" "${OPENCODE_PACKAGE_DIRECTORY}/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities" "${OPENCODE_PACKAGE_DIRECTORY}/capabilities"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance" "${OPENCODE_PACKAGE_DIRECTORY}/provenance"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/package/notices" "${OPENCODE_PACKAGE_DIRECTORY}/notices"
    COMMAND "${CMAKE_COMMAND}" -E copy
        "${CMAKE_CURRENT_BINARY_DIR}/generated/logonomic-cli-addon.json" "${OPENCODE_PACKAGE_DIRECTORY}/logonomic-cli-addon.json"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:opencode_adapter>"
        "${OPENCODE_PACKAGE_DIRECTORY}/adapter/opencode_adapter${OPENCODE_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:opencode_helper>"
        "${OPENCODE_PACKAGE_DIRECTORY}/helpers/opencode_helper${OPENCODE_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:opencode_launch_bridge>"
        "${OPENCODE_PACKAGE_DIRECTORY}/helpers/opencode_launch_bridge${OPENCODE_EXECUTABLE_SUFFIX}"
    COMMAND "$<TARGET_FILE:logonomic_cli_addon_index>" "${OPENCODE_PACKAGE_DIRECTORY}"
        --third-party "assets/opencode.svg" "notices/lobe_icons_mit.txt"
            "provenance/lobe_icons.json"
        --third-party "assets/opencode-white.svg" "notices/lobe_icons_mit.txt"
            "provenance/lobe_icons.json"
    DEPENDS opencode_adapter opencode_helper opencode_launch_bridge logonomic_cli_addon_index
        "${CMAKE_CURRENT_BINARY_DIR}/generated/logonomic-cli-addon.json" "${CMAKE_CURRENT_BINARY_DIR}/package-inputs.txt" ${package_files}
    VERBATIM)
add_custom_target(opencode_package ALL DEPENDS "${OPENCODE_PACKAGE_DIRECTORY}/package.files.json")
logonomic_cli_addon_install_package("${OPENCODE_PACKAGE_DIRECTORY}" logonomic.cli.opencode TARGET opencode_package)

# The tests belong to the package's own build. A build that embeds the package
# stages it and runs tests of its own, so nothing below is added there.
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()
enable_testing()
add_executable(opencode_preparation_tests tests/opencode_preparation_tests.cpp)
target_link_libraries(opencode_preparation_tests PRIVATE opencode_provider)
opencode_target(opencode_preparation_tests)
add_dependencies(opencode_preparation_tests opencode_package)
add_test(NAME opencode_preparation_tests COMMAND opencode_preparation_tests "${OPENCODE_PACKAGE_DIRECTORY}")
add_executable(opencode_acp_tests tests/opencode_acp_tests.cpp)
target_link_libraries(opencode_acp_tests PRIVATE opencode_provider)
opencode_target(opencode_acp_tests)
add_test(NAME opencode_acp_tests COMMAND opencode_acp_tests)
add_executable(opencode_lifecycle_tests tests/opencode_lifecycle_tests.cpp)
target_link_libraries(opencode_lifecycle_tests PRIVATE opencode_provider)
opencode_target(opencode_lifecycle_tests)
add_test(NAME opencode_lifecycle_tests COMMAND opencode_lifecycle_tests)
add_executable(opencode_launcher_tests tests/opencode_launcher_tests.cpp)
target_link_libraries(opencode_launcher_tests PRIVATE opencode_provider)
opencode_target(opencode_launcher_tests)
add_test(NAME opencode_launcher_tests COMMAND opencode_launcher_tests)
add_executable(opencode_fake_vnmctl tests/opencode_fake_vnmctl.cpp)
opencode_target(opencode_fake_vnmctl)
find_program(OPENCODE_NODE_EXECUTABLE node REQUIRED)
add_test(NAME opencode_plugin_tests COMMAND "${OPENCODE_NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/opencode_plugin_tests.mjs"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/assets/logonomic_opencode_plugin.mjs"
    "$<TARGET_FILE:opencode_helper>" "$<TARGET_FILE:opencode_fake_vnmctl>")
add_test(NAME opencode_helper_tests COMMAND "${OPENCODE_NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/opencode_helper_tests.mjs"
    "$<TARGET_FILE:opencode_helper>" "$<TARGET_FILE:opencode_fake_vnmctl>")
add_test(NAME opencode_helper_cancellation_tests COMMAND "${OPENCODE_NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/opencode_helper_cancellation_tests.mjs"
    "$<TARGET_FILE:opencode_helper>" "$<TARGET_FILE:opencode_fake_vnmctl>")
add_test(NAME opencode_plugin_disposal_tests COMMAND "${OPENCODE_NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/opencode_plugin_disposal_tests.mjs"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/assets/logonomic_opencode_plugin.mjs"
    "$<TARGET_FILE:opencode_helper>" "$<TARGET_FILE:opencode_fake_vnmctl>")
set_tests_properties(opencode_plugin_disposal_tests PROPERTIES TIMEOUT 125 LABELS slow)
add_test(NAME opencode_package_validation COMMAND logonomic_cli_addon_validate
    "${OPENCODE_PACKAGE_DIRECTORY}" --platform "${OPENCODE_PLATFORM}")
add_test(NAME opencode_package_conformance COMMAND logonomic_cli_addon_conformance
    "${OPENCODE_PACKAGE_DIRECTORY}")
set_tests_properties(
    opencode_preparation_tests opencode_acp_tests opencode_lifecycle_tests opencode_launcher_tests
    opencode_plugin_tests opencode_helper_tests opencode_helper_cancellation_tests
    opencode_package_validation opencode_package_conformance
    PROPERTIES TIMEOUT 120)

# Exercise this repository's actual producer, never a substitute packaging
# implementation. The isolated nested configure uses this same SDK source;
# it does not change the owned master dependency or edit an installed package.
option(LOGONOMIC_CLI_ADDON_RECIPE_TESTS
    "Register isolated package rebuild regressions (requires Python 3)" OFF)
if(LOGONOMIC_CLI_ADDON_RECIPE_TESTS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    get_target_property(_membership_sdk_source logonomic_cli_addon_sdk::sdk SOURCE_DIR)
    add_test(NAME opencode_package_membership
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_membership_tests.py"
            "--source=${CMAKE_CURRENT_SOURCE_DIR}" "--sdk=${_membership_sdk_source}"
            "--cmake=${CMAKE_COMMAND}" "--target=opencode_package" "--package-id=logonomic.cli.opencode"
            "--generator=${CMAKE_GENERATOR}" "--config=$<CONFIG>" "--build-type=${CMAKE_BUILD_TYPE}"
            "--generator-platform=${CMAKE_GENERATOR_PLATFORM}" "--toolset=${CMAKE_GENERATOR_TOOLSET}"
            "--toolchain=${CMAKE_TOOLCHAIN_FILE}" "--compiler=${CMAKE_CXX_COMPILER}"
            "--make-program=${CMAKE_MAKE_PROGRAM}" "--prefix-path=${CMAKE_PREFIX_PATH}"
            "--validator=$<TARGET_FILE:logonomic_cli_addon_validate>" "--platform=${OPENCODE_PLATFORM}")
    set_tests_properties(opencode_package_membership PROPERTIES
        TIMEOUT 1800 RESOURCE_LOCK cli_addon_recipe_rebuild LABELS "cli-addon;delivery;recipe;owning-path")
endif()
# Contract and recipe tests must not be confused with real native qualification.
get_directory_property(_provider_tests TESTS)
set_property(TEST ${_provider_tests} APPEND PROPERTY LABELS "cli-addon;provider-contract")
