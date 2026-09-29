cmake_minimum_required(VERSION 3.24)
project(VRControllerFreeze VERSION 1.0.0 DESCRIPTION "VR Controller Freeze" LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "VR Controller Freeze currently targets 64-bit Windows and SteamVR only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "VR Controller Freeze requires a 64-bit build.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "VR Controller Freeze currently requires MSVC.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
# SOURCE_SUBDIR names a folder that does not exist, so neither library's own CMake project is built:
# OpenVR ships prebuilt libraries and MinHook's sources are compiled directly into the driver.
FetchContent_Declare(
    openvr
    URL https://github.com/ValveSoftware/openvr/archive/refs/tags/v2.15.6.zip
    URL_HASH SHA256=7629e6586338dbc0f877571870f04534e24ee752126606c5d8ffdd7c3e7d31d4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _no_subproject
)
FetchContent_Declare(
    minhook
    URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip
    URL_HASH SHA256=172708123daa0c98d20d3a980b16a50be14af243dc95dee6f79c24193ad010e4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _no_subproject
)
FetchContent_MakeAvailable(openvr minhook)
set(OPENVR_INCLUDE_DIR "${openvr_SOURCE_DIR}/headers")
set(OPENVR_LIBRARY "${openvr_SOURCE_DIR}/lib/win64/openvr_api.lib")
set(OPENVR_DLL "${openvr_SOURCE_DIR}/bin/win64/openvr_api.dll")
foreach(required_file IN ITEMS "${OPENVR_LIBRARY}" "${OPENVR_DLL}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "OpenVR SDK file not found: ${required_file}")
    endif()
endforeach()

set(MINHOOK_SOURCES
    "${minhook_SOURCE_DIR}/src/hook.c"
    "${minhook_SOURCE_DIR}/src/buffer.c"
    "${minhook_SOURCE_DIR}/src/trampoline.c"
    "${minhook_SOURCE_DIR}/src/hde/hde64.c"
)
set_source_files_properties(${MINHOOK_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0")

# Version resources: src/version.rc.in, configured per binary with the project version.
function(configure_version_resource output description internal_name file_name file_type extra)
    set(RC_DESCRIPTION "${description}")
    set(RC_INTERNAL_NAME "${internal_name}")
    set(RC_FILE_NAME "${file_name}")
    set(RC_FILE_TYPE "${file_type}")
    set(RC_EXTRA "${extra}")
    configure_file("${CMAKE_SOURCE_DIR}/src/version.rc.in" "${output}" @ONLY)
endfunction()
configure_version_resource("${CMAKE_BINARY_DIR}/driver_version.rc" "VR Controller Freeze SteamVR driver"
    "driver_vrcontrollerfreeze" "driver_vrcontrollerfreeze.dll" "VFT_DLL" "")
configure_version_resource("${CMAKE_BINARY_DIR}/app_version.rc" "VR Controller Freeze"
    "VRControllerFreeze" "VRControllerFreeze.exe" "VFT_APP"
    "#include \"resource.h\"\nIDI_APP_ICON ICON \"${CMAKE_SOURCE_DIR}/assets/icon/VRControllerFreeze.ico\"")

# Strict warnings for this project's own C++ only; third-party headers are treated as external.
set(PROJECT_CXX_OPTIONS $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /EHsc $<$<CONFIG:Release>:/WX> /external:anglebrackets /external:W0>)

add_library(driver_vrcontrollerfreeze SHARED
    src/driver/driver_factory.cpp
    src/driver/device_provider.cpp
    src/driver/pose_hook.cpp
    src/driver/device_provider.h
    src/driver/pose_hook.h
    src/common/shared_state.h
    src/common/win_shared_memory.h
    "${CMAKE_BINARY_DIR}/driver_version.rc"
    ${MINHOOK_SOURCES}
)
target_include_directories(driver_vrcontrollerfreeze SYSTEM PRIVATE "${OPENVR_INCLUDE_DIR}" "${minhook_SOURCE_DIR}/include")
target_link_libraries(driver_vrcontrollerfreeze PRIVATE advapi32)
target_compile_definitions(driver_vrcontrollerfreeze PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(driver_vrcontrollerfreeze PRIVATE ${PROJECT_CXX_OPTIONS} /guard:cf)
target_link_options(driver_vrcontrollerfreeze PRIVATE $<$<CONFIG:Release>:/guard:cf> $<$<CONFIG:Release>:/DYNAMICBASE> $<$<CONFIG:Release>:/NXCOMPAT>)
set_target_properties(driver_vrcontrollerfreeze PROPERTIES OUTPUT_NAME "driver_vrcontrollerfreeze" PREFIX "")

# The app's decision logic, shared with its unit tests.
set(APP_LOGIC_SOURCES src/app/hand_logic.cpp src/app/hand_logic.h)

add_executable(VRControllerFreeze WIN32
    src/app/main.cpp
    src/app/app.cpp
    src/app/app.h
    src/app/app_log.cpp
    src/app/app_log.h
    src/app/driver_link.cpp
    src/app/driver_link.h
    src/app/main_window.cpp
    src/app/main_window.h
    src/app/steamvr_client.cpp
    src/app/steamvr_client.h
    src/app/resource.h
    src/app/settings.cpp
    src/app/settings.h
    src/app/win_util.h
    src/app/app.manifest
    "${CMAKE_BINARY_DIR}/app_version.rc"
    ${APP_LOGIC_SOURCES}
    src/common/shared_state.h
    src/common/win_shared_memory.h
)
target_include_directories(VRControllerFreeze PRIVATE src/app)
target_include_directories(VRControllerFreeze SYSTEM PRIVATE "${OPENVR_INCLUDE_DIR}")
target_link_libraries(VRControllerFreeze PRIVATE "${OPENVR_LIBRARY}" user32 shell32 advapi32 comctl32 dwmapi uxtheme gdiplus)
target_compile_definitions(VRControllerFreeze PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    APP_VERSION="${PROJECT_VERSION}")
target_compile_options(VRControllerFreeze PRIVATE ${PROJECT_CXX_OPTIONS} /guard:cf)
target_link_options(VRControllerFreeze PRIVATE $<$<CONFIG:Release>:/guard:cf> $<$<CONFIG:Release>:/DYNAMICBASE> $<$<CONFIG:Release>:/NXCOMPAT>)

set(DIST_DIR "${CMAKE_BINARY_DIR}/dist")
set(DRIVER_DIST_DIR "${DIST_DIR}/driver/vrcontrollerfreeze")
set(DRIVER_BIN_DIR "${DRIVER_DIST_DIR}/bin/win64")
# build/dist is a complete release: the app, the driver, and the scripts that install and remove it,
# laid out as in the project so the same launchers work from both.
add_custom_target(stage ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/input"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/tools"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DRIVER_BIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/install-driver.cmd" "${CMAKE_SOURCE_DIR}/uninstall-driver.cmd"
        "${CMAKE_SOURCE_DIR}/collect-diagnostics.cmd" "${DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/tools/install-driver.ps1" "${CMAKE_SOURCE_DIR}/tools/uninstall-driver.ps1"
        "${CMAKE_SOURCE_DIR}/tools/collect-diagnostics.ps1" "${CMAKE_SOURCE_DIR}/tools/steamvr-paths.ps1" "${DIST_DIR}/tools"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:VRControllerFreeze>" "${DIST_DIR}/VRControllerFreeze.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENVR_DLL}" "${DIST_DIR}/openvr_api.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/assets/vrcontrollerfreeze.vrmanifest" "${DIST_DIR}/vrcontrollerfreeze.vrmanifest"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/assets/input/actions.json" "${DIST_DIR}/input/actions.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/assets/input/bindings_oculus_touch.json" "${DIST_DIR}/input/bindings_oculus_touch.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:driver_vrcontrollerfreeze>" "${DRIVER_BIN_DIR}/driver_vrcontrollerfreeze.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/driver/driver.vrdrivermanifest" "${DRIVER_DIST_DIR}/driver.vrdrivermanifest"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/LICENSE" "${DIST_DIR}/LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${openvr_SOURCE_DIR}/LICENSE" "${DIST_DIR}/THIRD_PARTY_OPENVR_LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${minhook_SOURCE_DIR}/LICENSE.txt" "${DIST_DIR}/THIRD_PARTY_MINHOOK_LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/README.md" "${DIST_DIR}/README.md"
    DEPENDS VRControllerFreeze driver_vrcontrollerfreeze
    VERBATIM
)

# Tests are not part of the default build; tools/build.ps1 -Test builds and runs them.

# Unit tests for the app's decision logic.
add_executable(app_logic_tests EXCLUDE_FROM_ALL tests/app_logic_tests.cpp ${APP_LOGIC_SOURCES})
target_include_directories(app_logic_tests PRIVATE src/app)
target_compile_definitions(app_logic_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(app_logic_tests PRIVATE ${PROJECT_CXX_OPTIONS})

# Functional test: loads the built driver into a fake vrserver host.
add_executable(driver_harness EXCLUDE_FROM_ALL tests/driver_harness.cpp ${MINHOOK_SOURCES})
target_include_directories(driver_harness SYSTEM PRIVATE "${OPENVR_INCLUDE_DIR}" "${minhook_SOURCE_DIR}/include")
target_include_directories(driver_harness PRIVATE src/common)
target_link_libraries(driver_harness PRIVATE advapi32)
target_compile_definitions(driver_harness PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(driver_harness PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3 /EHsc /external:anglebrackets /external:W0>)
add_dependencies(driver_harness stage)
