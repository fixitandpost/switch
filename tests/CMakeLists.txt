include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.14.0
  EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(Catch2)

add_executable(
  switch-automation-tests
  ${CMAKE_CURRENT_SOURCE_DIR}/test-automation-model.cpp
  ${catch2_SOURCE_DIR}/extras/catch_amalgamated.cpp
  ${PROJECT_SOURCE_DIR}/src/automation/switch-automation-model.cpp
  ${PROJECT_SOURCE_DIR}/src/automation/switch-automation-engine.cpp
  ${PROJECT_SOURCE_DIR}/src/motion/switch-motion-manager.cpp
  ${PROJECT_SOURCE_DIR}/src/automation/switch-automation-engine.hpp
  ${PROJECT_SOURCE_DIR}/src/motion/switch-motion-manager.hpp
  ${PROJECT_SOURCE_DIR}/src/automation/switch-osc.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/switch-canvas-manager-stub.cpp
)

target_include_directories(
  switch-automation-tests
  PRIVATE ${PROJECT_SOURCE_DIR}/src/module
          ${PROJECT_SOURCE_DIR}/src/workspace
          ${PROJECT_SOURCE_DIR}/src/vertical
          ${PROJECT_SOURCE_DIR}/src/motion
          ${PROJECT_SOURCE_DIR}/src/automation
          ${PROJECT_SOURCE_DIR}/src/remote
          ${PROJECT_SOURCE_DIR}/src/ui
          ${PROJECT_SOURCE_DIR}/src/obs
          ${CMAKE_CURRENT_BINARY_DIR}
          ${PROJECT_BINARY_DIR}
          ${catch2_SOURCE_DIR}/extras
)

target_link_libraries(
  switch-automation-tests
  PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Network Qt6::Widgets
)

target_compile_options(
  switch-automation-tests PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-shorten-64-to-32>
)
if(OS_MACOS)
  target_link_options(
    switch-automation-tests
    PRIVATE "-Wl,-rpath,${PROJECT_SOURCE_DIR}/.deps/Frameworks"
            "-Wl,-rpath,${PROJECT_SOURCE_DIR}/.deps/obs-deps-2025-08-23-universal/lib"
            "-Wl,-rpath,${PROJECT_SOURCE_DIR}/.deps/obs-deps-qt6-2025-08-23-universal/lib"
  )
endif()

set_target_properties(
  switch-automation-tests
  PROPERTIES AUTOMOC ON
             XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
             XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
)

if(OS_MACOS)
  add_custom_command(
    TARGET switch-automation-tests
    POST_BUILD
    COMMAND /usr/bin/codesign --force --sign - "$<TARGET_FILE:switch-automation-tests>"
    VERBATIM
  )
endif()

if(OS_WINDOWS)
  set(
    SWITCH_WINDOWS_TEST_PATH_MODS
    "PATH=path_list_prepend:${PROJECT_SOURCE_DIR}/.deps/bin/64bit"
    "PATH=path_list_prepend:${PROJECT_SOURCE_DIR}/.deps/obs-deps-2025-08-23-x64/bin"
    "PATH=path_list_prepend:${PROJECT_SOURCE_DIR}/.deps/obs-deps-qt6-2025-08-23-x64/bin"
  )
endif()

add_test(NAME switch-automation-tests COMMAND switch-automation-tests)
if(OS_MACOS)
  set_tests_properties(
    switch-automation-tests
    PROPERTIES ENVIRONMENT
               "DYLD_FRAMEWORK_PATH=${PROJECT_SOURCE_DIR}/.deps/Frameworks:${PROJECT_SOURCE_DIR}/.deps/obs-deps-qt6-2025-08-23-universal/lib;DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/.deps/lib:${PROJECT_SOURCE_DIR}/.deps/obs-deps-2025-08-23-universal/lib"
  )
elseif(OS_WINDOWS)
  set_tests_properties(
    switch-automation-tests
    PROPERTIES ENVIRONMENT_MODIFICATION "${SWITCH_WINDOWS_TEST_PATH_MODS}"
  )
endif()

add_test(
  NAME switch-locale-key-parity
  COMMAND
    "${CMAKE_COMMAND}" "-DLOCALE_DIR=${PROJECT_SOURCE_DIR}/data/locale"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check-locale-keys.cmake"
)

add_test(
  NAME switch-vendor-api-contract
  COMMAND
    "${CMAKE_COMMAND}" "-DMODULE_SOURCE=${PROJECT_SOURCE_DIR}/src/module/switcher-vendor-api.inc"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check-vendor-api-contract.cmake"
)
