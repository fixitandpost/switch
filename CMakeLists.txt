cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

include(compilerconfig)
include(defaults)
include(helpers)
include(CTest)

add_library(${CMAKE_PROJECT_NAME} MODULE)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(SIMDe REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

include(FetchContent)
set(SWITCH_ONNXRUNTIME_VERSION 1.17.1)

if(OS_MACOS)
  FetchContent_Declare(
    onnxruntime
    URL "https://github.com/microsoft/onnxruntime/releases/download/v${SWITCH_ONNXRUNTIME_VERSION}/onnxruntime-osx-universal2-${SWITCH_ONNXRUNTIME_VERSION}.tgz"
  )
  FetchContent_MakeAvailable(onnxruntime)
elseif(OS_WINDOWS)
  FetchContent_Declare(
    onnxruntime_directml
    URL "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/${SWITCH_ONNXRUNTIME_VERSION}/microsoft.ml.onnxruntime.directml.${SWITCH_ONNXRUNTIME_VERSION}.nupkg"
    URL_HASH SHA256=834e9f02a348be0ae0fdf0e71df59661b64072e6f89fd6da19bcf74765f6574e
  )
  FetchContent_MakeAvailable(onnxruntime_directml)
  set(SWITCH_ONNXRUNTIME_DIRECTML_INCLUDE_DIR "${onnxruntime_directml_SOURCE_DIR}/build/native/include")
  set(SWITCH_ONNXRUNTIME_DIRECTML_NATIVE_DIR "${onnxruntime_directml_SOURCE_DIR}/runtimes/win-x64/native")
endif()

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE OBS::libobs OBS::obs-frontend-api SIMDe::SIMDe Qt6::Core Qt6::Widgets Qt6::Network
)

set(
  SWITCH_SOURCE_DIRS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/module"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/workspace"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/vertical"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/motion"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/automation"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/remote"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ui"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/obs"
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${SWITCH_SOURCE_DIRS} "${CMAKE_CURRENT_BINARY_DIR}")

if(OS_MACOS)
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${onnxruntime_SOURCE_DIR}/include)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.dylib "-framework Metal" "-framework CoreVideo" "-framework CoreML")
  target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "-Wl,-rpath,@loader_path/../Frameworks")
  set_property(TARGET ${CMAKE_PROJECT_NAME} APPEND PROPERTY BUILD_RPATH "@loader_path/../Frameworks")
  set_property(TARGET ${CMAKE_PROJECT_NAME} APPEND PROPERTY INSTALL_RPATH "@loader_path/../Frameworks")
elseif(OS_WINDOWS)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${SWITCH_ONNXRUNTIME_DIRECTML_INCLUDE_DIR}")
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "${SWITCH_ONNXRUNTIME_DIRECTML_NATIVE_DIR}/onnxruntime.lib")
endif()

if(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
  find_package(Qt6 REQUIRED COMPONENTS Gui)
endif()

if((OS_LINUX OR OS_FREEBSD OR OS_OPENBSD) AND Qt6_VERSION VERSION_LESS "6.9.0")
  find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::GuiPrivate)
endif()

target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h)

if(OS_WINDOWS)
  configure_file(cmake/windows/resources/installer-Windows.iss.in "${CMAKE_CURRENT_BINARY_DIR}/installer-Windows.generated.iss")
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/module/switcher-dock.cpp
          src/module/switcher-vendor-api.inc
          src/workspace/switcher-workspace.cpp
          src/ui/switcher-ui.cpp
          src/automation/switch-automation-model.cpp
          src/automation/switch-osc.cpp
          src/vertical/switch-canvas-manager.cpp
          src/automation/switch-automation-engine.cpp
          src/motion/switch-motion-manager.cpp
          src/remote/switcher-remote-manager.cpp
          src/motion/switch-ai-tracker.cpp
          src/remote/switcher-remote-renderer.cpp
          src/ui/qt-display.cpp
          src/ui/media-control.cpp
          src/ui/media-slider.cpp
          src/ui/volume-meter.cpp
          src/ui/slider-absoluteset-style.cpp
          src/module/switcher-dock.hpp
          src/workspace/switcher-workspace.hpp
          src/ui/switcher-ui.hpp
          src/automation/switch-automation-model.hpp
          src/automation/switch-osc.hpp
          src/vertical/switch-canvas-manager.hpp
          src/automation/switch-automation-engine.hpp
          src/motion/switch-motion-manager.hpp
          src/remote/switcher-remote-manager.hpp
          src/motion/switch-ai-tracker.hpp
          src/remote/switcher-remote-renderer.hpp
          src/obs/obs-websocket-api.h
          src/ui/qt-display.hpp
          src/ui/media-control.hpp
          src/ui/media-slider.hpp
          src/ui/volume-meter.hpp
          src/ui/slider-absoluteset-style.hpp
          src/ui/display-helpers.hpp
          ${CMAKE_CURRENT_BINARY_DIR}/version.h
)

find_program(GO_EXECUTABLE go REQUIRED)

if(OS_WINDOWS)
  set(SWITCHER_REMOTE_HELPER_FILENAME "switch-remote-helper.exe")
else()
  set(SWITCHER_REMOTE_HELPER_FILENAME "switch-remote-helper")
endif()

if(OS_MACOS)
  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:${CMAKE_PROJECT_NAME}>/Contents/Frameworks"
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.1.17.1.dylib"
      "$<TARGET_BUNDLE_DIR:${CMAKE_PROJECT_NAME}>/Contents/Frameworks/libonnxruntime.1.17.1.dylib"
    COMMAND
      "/usr/bin/codesign" "--force" "--sign" "-" "--timestamp" "-o" "runtime"
      "$<TARGET_BUNDLE_DIR:${CMAKE_PROJECT_NAME}>/Contents/Frameworks/libonnxruntime.1.17.1.dylib"
    COMMENT "Copy ONNX Runtime into plugin bundle"
    VERBATIM
  )
elseif(OS_WINDOWS)
  install(FILES "${SWITCH_ONNXRUNTIME_DIRECTML_NATIVE_DIR}/onnxruntime.dll" DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit")
  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different "${SWITCH_ONNXRUNTIME_DIRECTML_NATIVE_DIR}/onnxruntime.dll"
      "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/onnxruntime.dll"
    COMMENT "Copy ONNX Runtime DirectML runtime to rundir"
    VERBATIM
  )
endif()

set(SWITCHER_REMOTE_HELPER_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/remote/${SWITCHER_REMOTE_HELPER_FILENAME}")

add_custom_command(
  OUTPUT "${SWITCHER_REMOTE_HELPER_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/remote"
  COMMAND "${GO_EXECUTABLE}" build -o "${SWITCHER_REMOTE_HELPER_OUTPUT}" .
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/remote/helper"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/remote/helper/go.mod" "${CMAKE_CURRENT_SOURCE_DIR}/src/remote/helper/main.go"
  COMMENT "Building Switch remote helper"
  VERBATIM
)

set_source_files_properties("${SWITCHER_REMOTE_HELPER_OUTPUT}" PROPERTIES GENERATED TRUE)

add_custom_target(switch-remote-helper ALL DEPENDS "${SWITCHER_REMOTE_HELPER_OUTPUT}")
add_dependencies(${CMAKE_PROJECT_NAME} switch-remote-helper)

if(OS_MACOS)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE "${SWITCHER_REMOTE_HELPER_OUTPUT}")
  set_property(SOURCE "${SWITCHER_REMOTE_HELPER_OUTPUT}" PROPERTY MACOSX_PACKAGE_LOCATION "Resources/remote")
  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:${CMAKE_PROJECT_NAME}>/Contents/Resources/remote"
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different "${SWITCHER_REMOTE_HELPER_OUTPUT}"
      "$<TARGET_BUNDLE_DIR:${CMAKE_PROJECT_NAME}>/Contents/Resources/remote/${SWITCHER_REMOTE_HELPER_FILENAME}"
    COMMENT "Copy Switch remote helper into plugin bundle"
    VERBATIM
  )
elseif(OS_WINDOWS)
  install(FILES "${SWITCHER_REMOTE_HELPER_OUTPUT}" DESTINATION "${CMAKE_PROJECT_NAME}/data/remote")
  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/${CMAKE_PROJECT_NAME}/remote"
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different "${SWITCHER_REMOTE_HELPER_OUTPUT}"
      "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/${CMAKE_PROJECT_NAME}/remote/${SWITCHER_REMOTE_HELPER_FILENAME}"
    COMMENT "Copy Switch remote helper to rundir"
    VERBATIM
  )
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
