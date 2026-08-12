cmake_minimum_required(VERSION 3.20)

project(
  hlolli_wg_analyzer_portable
  VERSION 1.1.0
  DESCRIPTION "Path-free audio analysis kernel"
  LANGUAGES C
)

option(
  HLOLLI_WG_ANALYZER_STRICT
  "Treat compiler warnings as errors and enable extra checks"
  ON
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(hlolli_wg_analyzer_portable STATIC
  ../src/analysis.c
  ../src/dsp.c
  ../src/features.c
  ../src/wav_reader.c)
target_include_directories(hlolli_wg_analyzer_portable
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_options(hlolli_wg_analyzer_portable PRIVATE
  -Wall
  -Wextra
  -Wpedantic)
if(HLOLLI_WG_ANALYZER_STRICT)
  target_compile_options(hlolli_wg_analyzer_portable PRIVATE
    -Werror
    -Wconversion
    -Wformat=2
    -Wshadow
    -Wstrict-prototypes)
endif()

add_executable(hwa_wasi_tests
  ../tests/portability_tests.c)
target_link_libraries(hwa_wasi_tests PRIVATE
  hlolli_wg_analyzer_portable
  m)
target_compile_definitions(hwa_wasi_tests PRIVATE
  HWA_WASM_REACTOR=1)
target_compile_options(hwa_wasi_tests PRIVATE
  -Wall
  -Wextra
  -Wpedantic)
target_link_options(hwa_wasi_tests PRIVATE
  -nostartfiles
  "LINKER:--no-entry"
  "LINKER:--export=hwa_portability_test")
set_target_properties(hwa_wasi_tests PROPERTIES SUFFIX ".wasm")
if(HLOLLI_WG_ANALYZER_STRICT)
  target_compile_options(hwa_wasi_tests PRIVATE
    -Werror
    -Wconversion
    -Wformat=2
    -Wshadow
    -Wstrict-prototypes)
endif()

include(CTest)
if(BUILD_TESTING)
  add_test(
    NAME wasm.portability
    COMMAND
      ${CMAKE_CROSSCOMPILING_EMULATOR}
      --invoke hwa_portability_test
      $<TARGET_FILE:hwa_wasi_tests>)
  set_tests_properties(wasm.portability PROPERTIES TIMEOUT 30)

  add_test(
    NAME wasm.imports
    COMMAND
      "${CMAKE_COMMAND}"
      -DHWA_WASM_MODULE=$<TARGET_FILE:hwa_wasi_tests>
      -DHWA_WASM_TOOLS=${HWA_WASM_TOOLS}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/check_wasm_imports.cmake")
  set_tests_properties(wasm.imports PROPERTIES TIMEOUT 30)
endif()
