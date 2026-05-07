cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(SIMDe REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE OBS::libobs OBS::obs-frontend-api SIMDe::SIMDe Qt6::Core Qt6::Widgets Qt6::Network
)

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
  PRIVATE switcher-dock.cpp
          switcher-workspace.cpp
          switcher-settings.cpp
          switcher-remote-manager.cpp
          switcher-remote-renderer.cpp
          qt-display.cpp
          media-control.cpp
          media-slider.cpp
          volume-meter.cpp
          slider-absoluteset-style.cpp
          switcher-dock.hpp
          switcher-workspace.hpp
          switcher-settings.hpp
          switcher-remote-manager.hpp
          switcher-remote-renderer.hpp
          obs-websocket-api.h
          qt-display.hpp
          media-control.hpp
          media-slider.hpp
          volume-meter.hpp
          slider-absoluteset-style.hpp
          ${CMAKE_CURRENT_BINARY_DIR}/version.h
)

find_program(GO_EXECUTABLE go REQUIRED)

if(OS_WINDOWS)
  set(SWITCHER_REMOTE_HELPER_FILENAME "switch-remote-helper.exe")
else()
  set(SWITCHER_REMOTE_HELPER_FILENAME "switch-remote-helper")
endif()

set(SWITCHER_REMOTE_HELPER_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/remote/${SWITCHER_REMOTE_HELPER_FILENAME}")

add_custom_command(
  OUTPUT "${SWITCHER_REMOTE_HELPER_OUTPUT}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/remote"
  COMMAND "${GO_EXECUTABLE}" build -o "${SWITCHER_REMOTE_HELPER_OUTPUT}" .
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/remote-helper"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/remote-helper/go.mod" "${CMAKE_CURRENT_SOURCE_DIR}/remote-helper/main.go"
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
