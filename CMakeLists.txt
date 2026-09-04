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

option(
  HLOLLI_WG_ANALYZER_ENABLE_ONNX
  "Link ONNX Runtime into the analyzer CLI"
  OFF
)

set(
  HLOLLI_WG_ANALYZER_BASIC_PITCH_TEST_MODEL
  ""
  CACHE FILEPATH
  "Pinned Basic Pitch ONNX model used by the opt-in integration test"
)

set(
  HLOLLI_WG_ANALYZER_HTDEMUCS_TEST_MODEL
  ""
  CACHE FILEPATH
  "Pinned HTDemucs six-stem ONNX model used by the opt-in integration test"
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(hlolli_wg_analyzer_core STATIC
  src/alignment.c
  src/alignment_file.c
  src/analysis.c
  src/analysis_file.c
  src/basic_pitch.c
  src/basic_pitch_audio_provider.c
  src/basic_pitch_provider.c
  src/body_envelope.c
  src/dsp.c
  src/experiment.c
  src/experiment_file.c
  src/event_analysis.c
  src/event_bundle.c
  src/event_csound.c
  src/features.c
  src/gap_report.c
  src/gap_report_clip.c
  src/gap_report_file.c
  src/gap_report_output.c
  src/harmonic_decay.c
  src/htdemucs.c
  src/inference.c
  src/inference_clock.c
  src/inference_provenance.c
  src/instrument_stem_provider.c
  src/item_file.c
  src/isolated_note.c
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
  src/stem_note_derivation.c
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
  src/harmonic_decay_report.c
  src/inference_basic_pitch_onnx.c
  src/inference_htdemucs_onnx.c
  src/inference_onnx.c
  src/item_report.c
  src/isolated_note_report.c
  src/main.c
  src/measure_report.c
  src/physical_report.c
  src/production_report.c
  src/output.c
  src/report.c
  src/run_report.c
)
target_link_libraries(hlolli_wg_analyzer PRIVATE hlolli_wg_analyzer_core)

set(HWA_BASIC_PITCH_ADAPTER_HASH_INPUT
  "org.hlolli.basic-pitch-onnx|1"
)
foreach(HWA_BASIC_PITCH_ADAPTER_FILE IN ITEMS
    src/basic_pitch.c
    src/basic_pitch.h
    src/basic_pitch_provider.c
    src/basic_pitch_provider.h
    src/inference_basic_pitch_onnx.c
    src/inference_basic_pitch_onnx.h
    src/inference_clock.c
    src/inference_clock.h
    src/inference_provenance.c
    src/inference_provenance.h)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/${HWA_BASIC_PITCH_ADAPTER_FILE}"
  )
  file(SHA256
    "${CMAKE_CURRENT_SOURCE_DIR}/${HWA_BASIC_PITCH_ADAPTER_FILE}"
    HWA_BASIC_PITCH_ADAPTER_FILE_SHA256)
  string(APPEND HWA_BASIC_PITCH_ADAPTER_HASH_INPUT
    "|${HWA_BASIC_PITCH_ADAPTER_FILE}:${HWA_BASIC_PITCH_ADAPTER_FILE_SHA256}")
endforeach()
string(SHA256 HWA_BASIC_PITCH_ADAPTER_SHA256
  "${HWA_BASIC_PITCH_ADAPTER_HASH_INPUT}"
)
unset(HWA_BASIC_PITCH_ADAPTER_FILE_SHA256)
unset(HWA_BASIC_PITCH_ADAPTER_FILE)
unset(HWA_BASIC_PITCH_ADAPTER_HASH_INPUT)
target_compile_definitions(hlolli_wg_analyzer PRIVATE
  HWA_BASIC_PITCH_ADAPTER_SHA256="${HWA_BASIC_PITCH_ADAPTER_SHA256}"
)

set(HWA_BASIC_PITCH_AUDIO_ADAPTER_HASH_INPUT
  "org.hlolli.basic-pitch-audio-provider|1"
)
foreach(HWA_BASIC_PITCH_AUDIO_ADAPTER_FILE IN ITEMS
    src/basic_pitch_audio_provider.c
    src/basic_pitch_audio_provider.h
    src/inference_clock.c
    src/inference_clock.h)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/${HWA_BASIC_PITCH_AUDIO_ADAPTER_FILE}"
  )
  file(SHA256
    "${CMAKE_CURRENT_SOURCE_DIR}/${HWA_BASIC_PITCH_AUDIO_ADAPTER_FILE}"
    HWA_BASIC_PITCH_AUDIO_ADAPTER_FILE_SHA256)
  string(APPEND HWA_BASIC_PITCH_AUDIO_ADAPTER_HASH_INPUT
    "|${HWA_BASIC_PITCH_AUDIO_ADAPTER_FILE}:${HWA_BASIC_PITCH_AUDIO_ADAPTER_FILE_SHA256}")
endforeach()
string(SHA256 HWA_BASIC_PITCH_AUDIO_ADAPTER_SHA256
  "${HWA_BASIC_PITCH_AUDIO_ADAPTER_HASH_INPUT}"
)
unset(HWA_BASIC_PITCH_AUDIO_ADAPTER_FILE_SHA256)
unset(HWA_BASIC_PITCH_AUDIO_ADAPTER_FILE)
unset(HWA_BASIC_PITCH_AUDIO_ADAPTER_HASH_INPUT)
target_compile_definitions(hlolli_wg_analyzer PRIVATE
  HWA_BASIC_PITCH_AUDIO_ADAPTER_SHA256="${HWA_BASIC_PITCH_AUDIO_ADAPTER_SHA256}"
)

set(HWA_HTDEMUCS_ADAPTER_HASH_INPUT
  "org.hlolli.htdemucs-6s-onnx|1"
)
foreach(HWA_HTDEMUCS_ADAPTER_FILE IN ITEMS
    src/htdemucs.c
    src/htdemucs.h
    src/inference_htdemucs_onnx.c
    src/inference_htdemucs_onnx.h
    src/inference_clock.c
    src/inference_clock.h
    src/inference_provenance.c
    src/inference_provenance.h
    src/instrument_stem_provider.c
    src/instrument_stem_provider.h)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/${HWA_HTDEMUCS_ADAPTER_FILE}"
  )
  file(SHA256
    "${CMAKE_CURRENT_SOURCE_DIR}/${HWA_HTDEMUCS_ADAPTER_FILE}"
    HWA_HTDEMUCS_ADAPTER_FILE_SHA256)
  string(APPEND HWA_HTDEMUCS_ADAPTER_HASH_INPUT
    "|${HWA_HTDEMUCS_ADAPTER_FILE}:${HWA_HTDEMUCS_ADAPTER_FILE_SHA256}")
endforeach()
string(SHA256 HWA_HTDEMUCS_ADAPTER_SHA256
  "${HWA_HTDEMUCS_ADAPTER_HASH_INPUT}"
)
unset(HWA_HTDEMUCS_ADAPTER_FILE_SHA256)
unset(HWA_HTDEMUCS_ADAPTER_FILE)
unset(HWA_HTDEMUCS_ADAPTER_HASH_INPUT)
target_compile_definitions(hlolli_wg_analyzer PRIVATE
  HWA_HTDEMUCS_ADAPTER_SHA256="${HWA_HTDEMUCS_ADAPTER_SHA256}"
)

if(HLOLLI_WG_ANALYZER_ENABLE_ONNX)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  find_package(ONNXRuntime REQUIRED)
  target_compile_definitions(hlolli_wg_analyzer PRIVATE
    HWA_HAVE_ONNX_RUNTIME=1
  )
  target_link_libraries(hlolli_wg_analyzer PRIVATE
    ONNXRuntime::ONNXRuntime
  )
  if(NOT WIN32)
    set_target_properties(hlolli_wg_analyzer PROPERTIES
      INSTALL_RPATH_USE_LINK_PATH TRUE
    )
  endif()
  if(WIN32)
    add_custom_command(TARGET hlolli_wg_analyzer POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${ONNXRuntime_RUNTIME_LIBRARY}"
        "$<TARGET_FILE_DIR:hlolli_wg_analyzer>"
      VERBATIM
    )
  endif()
else()
  target_compile_definitions(hlolli_wg_analyzer PRIVATE
    HWA_HAVE_ONNX_RUNTIME=0
  )
endif()

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
install(FILES LICENSE
  DESTINATION "${CMAKE_INSTALL_DATADIR}/hlolli_wg_analyzer/licenses"
  RENAME LICENSE-MIT
)
install(FILES third_party/basic-pitch/LICENSE
  DESTINATION "${CMAKE_INSTALL_DATADIR}/hlolli_wg_analyzer/licenses"
  RENAME basic-pitch-Apache-2.0.txt
)
install(FILES third_party/basic-pitch/NOTICE
  DESTINATION "${CMAKE_INSTALL_DATADIR}/hlolli_wg_analyzer/licenses"
  RENAME basic-pitch-NOTICE.txt
)
install(FILES third_party/demucs/LICENSE
  DESTINATION "${CMAKE_INSTALL_DATADIR}/hlolli_wg_analyzer/licenses"
  RENAME demucs-MIT.txt
)
install(FILES third_party/demucs-onnx/LICENSE
  DESTINATION "${CMAKE_INSTALL_DATADIR}/hlolli_wg_analyzer/licenses"
  RENAME demucs-onnx-MIT.txt
)

include(CTest)
if(BUILD_TESTING AND HLOLLI_WG_ANALYZER_BASIC_PITCH_TEST_MODEL)
  set(HWA_BASIC_PITCH_REFERENCE_MODEL_SHA256
    "2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec"
  )
  get_filename_component(HWA_BASIC_PITCH_TEST_MODEL_PATH
    "${HLOLLI_WG_ANALYZER_BASIC_PITCH_TEST_MODEL}"
    ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
  )
  if(NOT HLOLLI_WG_ANALYZER_ENABLE_ONNX)
    message(FATAL_ERROR
      "HLOLLI_WG_ANALYZER_BASIC_PITCH_TEST_MODEL needs "
      "HLOLLI_WG_ANALYZER_ENABLE_ONNX=ON"
    )
  endif()
  if(NOT EXISTS "${HWA_BASIC_PITCH_TEST_MODEL_PATH}")
    message(FATAL_ERROR
      "Basic Pitch test model does not exist: "
      "${HWA_BASIC_PITCH_TEST_MODEL_PATH}"
    )
  endif()
  if(IS_DIRECTORY "${HWA_BASIC_PITCH_TEST_MODEL_PATH}")
    message(FATAL_ERROR
      "Basic Pitch test model is not a file: "
      "${HWA_BASIC_PITCH_TEST_MODEL_PATH}"
    )
  endif()
  file(SHA256
    "${HWA_BASIC_PITCH_TEST_MODEL_PATH}"
    HWA_BASIC_PITCH_TEST_MODEL_SHA256
  )
  if(NOT HWA_BASIC_PITCH_TEST_MODEL_SHA256 STREQUAL
      HWA_BASIC_PITCH_REFERENCE_MODEL_SHA256)
    message(FATAL_ERROR
      "Basic Pitch test model SHA-256 is "
      "${HWA_BASIC_PITCH_TEST_MODEL_SHA256}; expected "
      "${HWA_BASIC_PITCH_REFERENCE_MODEL_SHA256}"
    )
  endif()
endif()
if(BUILD_TESTING AND HLOLLI_WG_ANALYZER_HTDEMUCS_TEST_MODEL)
  set(HWA_HTDEMUCS_REFERENCE_MODEL_SHA256
    "7ce55792e2231c93fbf92de95f5fd5b3a5e6c89f7db690dfd693e8f1dce56869"
  )
  get_filename_component(HWA_HTDEMUCS_TEST_MODEL_PATH
    "${HLOLLI_WG_ANALYZER_HTDEMUCS_TEST_MODEL}"
    ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
  )
  if(NOT HLOLLI_WG_ANALYZER_ENABLE_ONNX)
    message(FATAL_ERROR
      "HLOLLI_WG_ANALYZER_HTDEMUCS_TEST_MODEL needs "
      "HLOLLI_WG_ANALYZER_ENABLE_ONNX=ON"
    )
  endif()
  if(NOT EXISTS "${HWA_HTDEMUCS_TEST_MODEL_PATH}")
    message(FATAL_ERROR
      "HTDemucs test model does not exist: "
      "${HWA_HTDEMUCS_TEST_MODEL_PATH}"
    )
  endif()
  if(IS_DIRECTORY "${HWA_HTDEMUCS_TEST_MODEL_PATH}")
    message(FATAL_ERROR
      "HTDemucs test model is not a file: "
      "${HWA_HTDEMUCS_TEST_MODEL_PATH}"
    )
  endif()
  file(SHA256
    "${HWA_HTDEMUCS_TEST_MODEL_PATH}"
    HWA_HTDEMUCS_TEST_MODEL_SHA256
  )
  if(NOT HWA_HTDEMUCS_TEST_MODEL_SHA256 STREQUAL
      HWA_HTDEMUCS_REFERENCE_MODEL_SHA256)
    message(FATAL_ERROR
      "HTDemucs test model SHA-256 is "
      "${HWA_HTDEMUCS_TEST_MODEL_SHA256}; expected "
      "${HWA_HTDEMUCS_REFERENCE_MODEL_SHA256}"
    )
  endif()
endif()
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
