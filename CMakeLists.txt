cmake_minimum_required(VERSION 3.20)

project(
  hlolli_wg_analyzer
  VERSION 1.1.0
  DESCRIPTION "Bounded read-only WAVE audio analysis"
  LANGUAGES C
)

option(
  HLOLLI_WG_ANALYZER_STRICT
  "Treat compiler warnings as errors and enable extra checks"
  OFF
)

option(
  HLOLLI_WG_ANALYZER_BUILD_BENCHMARKS
  "Build the opt-in benchmark harness"
  OFF
)

option(
  HLOLLI_WG_ANALYZER_BUILD_REGRESSION_TESTS
  "Build the opt-in malformed-input regression runner"
  OFF
)

option(
  HLOLLI_WG_ANALYZER_BUILD_FUZZERS
  "Build the opt-in libFuzzer targets"
  OFF
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(hlolli_wg_analyzer_core STATIC
  src/alignment.c
  src/alignment_file.c
  src/analysis.c
  src/analysis_file.c
  src/body_envelope.c
  src/dsp.c
  src/experiment.c
  src/experiment_file.c
  src/features.c
  src/gap_report.c
  src/gap_report_clip.c
  src/gap_report_file.c
  src/gap_report_output.c
  src/item_file.c
  src/measure_compare.c
  src/measure_engine.c
  src/measure_file.c
  src/numeric_locale.c
  src/physical_check.c
  src/physical_file.c
  src/production.c
  src/production_file.c
  src/run.c
  src/run_file.c
  src/score_manifest.c
  src/segmentation.c
  src/sha256.c
  src/typed_labels.c
  src/wav_reader.c
  src/wav_reader_file.c
)
target_include_directories(hlolli_wg_analyzer_core
  PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

function(hwa_set_warnings target_name)
  if(MSVC)
    target_compile_definitions(${target_name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target_name} PRIVATE /W4)
    if(HLOLLI_WG_ANALYZER_STRICT)
      target_compile_options(${target_name} PRIVATE /WX /permissive-)
    endif()
  else()
    target_compile_options(${target_name} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
    )
    if(HLOLLI_WG_ANALYZER_STRICT)
      target_compile_options(${target_name} PRIVATE
        -Werror
        -Wconversion
        -Wformat=2
        -Wshadow
        -Wstrict-prototypes
      )
    endif()
  endif()
endfunction()

hwa_set_warnings(hlolli_wg_analyzer_core)

if(NOT MSVC)
  target_link_libraries(hlolli_wg_analyzer_core PUBLIC m)
endif()

add_executable(hlolli_wg_analyzer
  src/alignment_report.c
  src/body_envelope_report.c
  src/experiment_process.c
  src/experiment_report.c
  src/file_output.c
  src/item_report.c
  src/main.c
  src/measure_report.c
  src/physical_report.c
  src/production_report.c
  src/output.c
  src/report.c
  src/run_report.c
)
target_link_libraries(hlolli_wg_analyzer PRIVATE hlolli_wg_analyzer_core)
set_target_properties(hlolli_wg_analyzer PROPERTIES
  OUTPUT_NAME "hlolli-wg-analyzer"
)
hwa_set_warnings(hlolli_wg_analyzer)

include(GNUInstallDirs)
install(TARGETS hlolli_wg_analyzer hlolli_wg_analyzer_core
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(FILES include/hlolli_wg_analyzer.h
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

include(CTest)
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  add_subdirectory(tests)
endif()

if(HLOLLI_WG_ANALYZER_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

if(HLOLLI_WG_ANALYZER_BUILD_REGRESSION_TESTS)
  add_subdirectory(regression)
endif()

if(HLOLLI_WG_ANALYZER_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()
