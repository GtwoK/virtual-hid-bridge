cmake_minimum_required(VERSION 3.20)
project(vhid_bridge LANGUAGES C CXX OBJCXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(vhid-core
    src/protocol.cpp
    src/mapping.cpp
    src/generic_hid_profile.cpp
    src/switch_pro_profile.cpp)
target_include_directories(vhid-core PUBLIC include)
target_compile_options(vhid-core PRIVATE -Wall -Wextra -Wpedantic)

if(APPLE)
    add_library(vhid-platform
        src/physical_hid_source_macos.mm
        src/virtual_device_macos.mm)
    target_include_directories(vhid-platform PUBLIC include)
    target_compile_options(vhid-platform PRIVATE -Wall -Wextra -Wpedantic -fblocks)
    target_link_libraries(vhid-platform PUBLIC
        "-framework CoreFoundation"
        "-framework IOKit")
else()
    message(FATAL_ERROR "The virtual-HID backend currently requires macOS")
endif()

add_executable(vhid-bridge
    src/main.cpp)
target_include_directories(vhid-bridge PRIVATE include)
target_compile_options(vhid-bridge PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(vhid-bridge PRIVATE vhid-core vhid-platform)

add_executable(vhid-bridge-ui MACOSX_BUNDLE
    src/ui_app.mm)
set_target_properties(vhid-bridge-ui PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Virtual HID Bridge"
    MACOSX_BUNDLE_GUI_IDENTIFIER "local.virtual-hid-bridge.app"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
    MACOSX_BUNDLE_BUNDLE_VERSION "1")
target_compile_options(vhid-bridge-ui PRIVATE -Wall -Wextra -Wpedantic -fobjc-arc)
target_link_libraries(vhid-bridge-ui PRIVATE
    "-framework AppKit"
    "-framework Foundation")
add_dependencies(vhid-bridge-ui vhid-bridge)
add_custom_target(vhid-bundle-helper ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:vhid-bridge>
        $<TARGET_BUNDLE_DIR:vhid-bridge-ui>/Contents/MacOS/vhid-bridge
    DEPENDS vhid-bridge vhid-bridge-ui
    COMMENT "Embedding vhid-bridge helper in vhid-bridge-ui.app")

find_program(CODESIGN_EXECUTABLE codesign)
if(CODESIGN_EXECUTABLE)
    set(VHID_CODE_SIGN_IDENTITY "-" CACHE STRING
        "Code signing identity for local Virtual HID Bridge builds")
    add_custom_target(vhid-sign-local
        COMMAND ${CODESIGN_EXECUTABLE} --force --sign ${VHID_CODE_SIGN_IDENTITY}
            --entitlements ${CMAKE_SOURCE_DIR}/vhid.entitlements
            $<TARGET_BUNDLE_DIR:vhid-bridge-ui>/Contents/MacOS/vhid-bridge
        COMMAND ${CODESIGN_EXECUTABLE} --force --sign ${VHID_CODE_SIGN_IDENTITY}
            $<TARGET_BUNDLE_DIR:vhid-bridge-ui>
        DEPENDS vhid-bundle-helper
        COMMENT "Signing vhid-bridge-ui.app and its entitled helper for local testing")
endif()

enable_testing()
add_executable(vhid-core-tests tests/core_tests.cpp)
target_link_libraries(vhid-core-tests PRIVATE vhid-core)
target_compile_options(vhid-core-tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME vhid-core-tests COMMAND vhid-core-tests)

add_executable(vhid-wire-c-tests tests/wire_c_tests.c)
target_include_directories(vhid-wire-c-tests PRIVATE include)
target_compile_options(vhid-wire-c-tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME vhid-wire-c-tests COMMAND vhid-wire-c-tests)
