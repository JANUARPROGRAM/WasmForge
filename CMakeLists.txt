cmake_minimum_required(VERSION 3.16)
project(WasmForge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(WASMFORGE_BUILD_TESTS "Build the unit tests" ON)
option(WASMFORGE_STATIC "Link the runtime statically (handy for a single-file wasmfrg.exe)" OFF)

if(WIN32)
  set(WF_PLATFORM_SRC src/platform/platform_win32.cpp)
else()
  set(WF_PLATFORM_SRC src/platform/platform_posix.cpp)
endif()

add_library(wasmforge_core STATIC
  src/core/util.cpp
  ${WF_PLATFORM_SRC}
  src/hardware/hardware.cpp
  src/project/project.cpp
  src/project/templates.cpp
  src/assets/assets.cpp
  src/config/config.cpp
  src/backend/backend.cpp
  src/build/build.cpp
  src/diagnostics/doctor.cpp
  src/runtime/wasm.cpp
  src/runtime/runner.cpp
  src/runtime/server.cpp
  src/cli/cli.cpp)
target_include_directories(wasmforge_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(wasmforge_core PUBLIC Threads::Threads)

if(WIN32)
  target_compile_definitions(wasmforge_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(wasmforge_core PUBLIC advapi32 shell32 ws2_32)
endif()
if(MSVC)
  target_compile_options(wasmforge_core PUBLIC /W4 /utf-8 /permissive-)
else()
  target_compile_options(wasmforge_core PUBLIC -Wall -Wextra)
endif()

add_executable(wasmfrg src/main.cpp)
target_link_libraries(wasmfrg PRIVATE wasmforge_core)
if(WASMFORGE_STATIC AND NOT MSVC)
  target_link_options(wasmfrg PRIVATE -static)
endif()
install(TARGETS wasmfrg RUNTIME DESTINATION bin)

if(WASMFORGE_BUILD_TESTS)
  enable_testing()
  add_executable(wf_unit_tests tests/unit/test_main.cpp)
  target_link_libraries(wf_unit_tests PRIVATE wasmforge_core)
  add_test(NAME unit COMMAND wf_unit_tests)
  find_program(WF_BASH bash)
  if(WF_BASH AND NOT WIN32)
    add_test(NAME integration COMMAND ${WF_BASH} ${CMAKE_SOURCE_DIR}/tests/integration/run.sh)
    set_tests_properties(integration PROPERTIES ENVIRONMENT "WASMFRG=$<TARGET_FILE:wasmfrg>")
  endif()
endif()
