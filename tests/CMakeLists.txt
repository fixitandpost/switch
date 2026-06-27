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
  ${CMAKE_CURRENT_SOURCE_DIR}/../switch-automation-model.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../switch-automation-engine.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../switch-motion-manager.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../switch-automation-engine.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../switch-motion-manager.hpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../switch-osc.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/switch-canvas-manager-stub.cpp
)

target_include_directories(
  switch-automation-tests
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
          ${catch2_SOURCE_DIR}/extras
)

target_link_libraries(
  switch-automation-tests
  PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Network Qt6::Widgets
)

target_compile_options(switch-automation-tests PRIVATE -Wno-shorten-64-to-32)
target_link_options(
  switch-automation-tests
  PRIVATE "-Wl,-rpath,${PROJECT_SOURCE_DIR}/.deps/Frameworks"
          "-Wl,-rpath,${PROJECT_SOURCE_DIR}/.deps/obs-deps-2025-08-23-universal/lib"
          "-Wl,-rpath,${PROJECT_SOURCE_DIR}/.deps/obs-deps-qt6-2025-08-23-universal/lib"
)

set_target_properties(
  switch-automation-tests
  PROPERTIES AUTOMOC ON
             XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
             XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
)

add_custom_command(
  TARGET switch-automation-tests
  POST_BUILD
  COMMAND /usr/bin/codesign --force --sign - "$<TARGET_FILE:switch-automation-tests>"
  VERBATIM
)

add_test(NAME switch-automation-tests COMMAND switch-automation-tests)
set_tests_properties(
  switch-automation-tests
  PROPERTIES ENVIRONMENT
             "DYLD_FRAMEWORK_PATH=${PROJECT_SOURCE_DIR}/.deps/Frameworks:${PROJECT_SOURCE_DIR}/.deps/obs-deps-qt6-2025-08-23-universal/lib;DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/.deps/lib:${PROJECT_SOURCE_DIR}/.deps/obs-deps-2025-08-23-universal/lib"
)
