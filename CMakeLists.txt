cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Do not capture the configure environment" FORCE)

project(logonomic_cli_opencode VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
    add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_BINARY_DIR}/sdk")
else()
    include(FetchContent)
    FetchContent_Declare(logonomic_sdk_source
        GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
        GIT_TAG master
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
    FetchContent_MakeAvailable(logonomic_sdk_source)
endif()

function(opencode_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
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
add_executable(opencode_launch_bridge src/opencode_launch_bridge.cpp)
opencode_target(opencode_launch_bridge)

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(OPENCODE_PLATFORM windows-x86_64)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(OPENCODE_PLATFORM linux-x86_64)
else()
    message(FATAL_ERROR "Package artifacts support Windows and Linux x86_64.")
endif()
set(OPENCODE_EXECUTABLE_SUFFIX "${CMAKE_EXECUTABLE_SUFFIX}")
set(OPENCODE_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package/logonomic.cli.opencode")
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
file(GLOB_RECURSE package_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/package/assets/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance/*")
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
    COMMAND "${CMAKE_COMMAND}" -E copy
        "${CMAKE_BINARY_DIR}/generated/logonomic-cli-addon.json" "${OPENCODE_PACKAGE_DIRECTORY}/logonomic-cli-addon.json"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:opencode_adapter>"
        "${OPENCODE_PACKAGE_DIRECTORY}/adapter/opencode_adapter${OPENCODE_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:opencode_helper>"
        "${OPENCODE_PACKAGE_DIRECTORY}/helpers/opencode_helper${OPENCODE_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:opencode_launch_bridge>"
        "${OPENCODE_PACKAGE_DIRECTORY}/helpers/opencode_launch_bridge${OPENCODE_EXECUTABLE_SUFFIX}"
    COMMAND "$<TARGET_FILE:logonomic_cli_addon_index>" "${OPENCODE_PACKAGE_DIRECTORY}"
    DEPENDS opencode_adapter opencode_helper opencode_launch_bridge logonomic_cli_addon_index
        "${CMAKE_BINARY_DIR}/generated/logonomic-cli-addon.json" ${package_files}
    VERBATIM)
add_custom_target(opencode_package ALL DEPENDS "${OPENCODE_PACKAGE_DIRECTORY}/package.files.json")
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
install(DIRECTORY "${OPENCODE_PACKAGE_DIRECTORY}/" DESTINATION logonomic.cli.opencode)
