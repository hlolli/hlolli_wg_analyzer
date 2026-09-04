add_executable(hlolli_wg_analyzer_cli_tests cli_tests.c)
target_compile_features(hlolli_wg_analyzer_cli_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_cli_tests)

add_executable(
  hlolli_wg_analyzer_body_envelope_tests
  body_envelope_tests.c)
target_include_directories(
  hlolli_wg_analyzer_body_envelope_tests PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_body_envelope_tests PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_body_envelope_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_body_envelope_tests)
add_test(
  NAME unit.body-envelope
  COMMAND hlolli_wg_analyzer_body_envelope_tests)
set_tests_properties(unit.body-envelope PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_event_bundle_tests
  event_bundle_tests.c)
target_include_directories(
  hlolli_wg_analyzer_event_bundle_tests PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_event_bundle_tests PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_event_bundle_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_event_bundle_tests)
add_test(
  NAME unit.event-bundle
  COMMAND hlolli_wg_analyzer_event_bundle_tests)
set_tests_properties(unit.event-bundle PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_event_bundle_conformance_tests
  event_bundle_conformance_tests.c)
target_include_directories(
  hlolli_wg_analyzer_event_bundle_conformance_tests PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_event_bundle_conformance_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_event_bundle_conformance_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_event_bundle_conformance_tests)
add_test(
  NAME unit.event-bundle-conformance
  COMMAND hlolli_wg_analyzer_event_bundle_conformance_tests)
set_tests_properties(unit.event-bundle-conformance PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_inference_tests
  inference_tests.c)
target_include_directories(
  hlolli_wg_analyzer_inference_tests PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_inference_tests PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_inference_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_inference_tests)
add_test(
  NAME unit.inference-provider
  COMMAND hlolli_wg_analyzer_inference_tests)
set_tests_properties(unit.inference-provider PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_inference_clock_tests
  inference_clock_tests.c)
target_include_directories(
  hlolli_wg_analyzer_inference_clock_tests PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_inference_clock_tests PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_inference_clock_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_inference_clock_tests)
add_test(
  NAME unit.inference-clock
  COMMAND hlolli_wg_analyzer_inference_clock_tests)
set_tests_properties(unit.inference-clock PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_inference_provenance_tests
  inference_provenance_tests.c)
target_include_directories(
  hlolli_wg_analyzer_inference_provenance_tests PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_inference_provenance_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_inference_provenance_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_inference_provenance_tests)
add_test(
  NAME unit.inference-provenance
  COMMAND hlolli_wg_analyzer_inference_provenance_tests)
set_tests_properties(unit.inference-provenance PROPERTIES TIMEOUT 30)

add_test(
  NAME cli.inference-capabilities-text
  COMMAND hlolli_wg_analyzer inference-capabilities)
set_tests_properties(
  cli.inference-capabilities-text PROPERTIES
  PASS_REGULAR_EXPRESSION
    "Instrument stem task: HTDemucs six-stem adapter (available|not available)"
  TIMEOUT 30)

add_test(
  NAME cli.inference-capabilities-json
  COMMAND hlolli_wg_analyzer --json inference-capabilities)
if(HLOLLI_WG_ANALYZER_ENABLE_ONNX)
  set(HWA_STEM_CAPABILITY_AVAILABLE "true")
else()
  set(HWA_STEM_CAPABILITY_AVAILABLE "false")
endif()
set_tests_properties(
  cli.inference-capabilities-json PROPERTIES
  PASS_REGULAR_EXPRESSION
    "\"schema\":\"hwa-inference-capabilities\".*\"contract\":\"org.hlolli.polyphonic-note-events-v1\".*\"contract\":\"org.hlolli.instrument-stems-v1\".*\"available\":${HWA_STEM_CAPABILITY_AVAILABLE}"
  TIMEOUT 30)
unset(HWA_STEM_CAPABILITY_AVAILABLE)

add_test(
  NAME cli.inference-capabilities-reject-analysis-option
  COMMAND hlolli_wg_analyzer --frame-size 256 inference-capabilities)
set_tests_properties(
  cli.inference-capabilities-reject-analysis-option PROPERTIES
  WILL_FAIL TRUE
  TIMEOUT 30)

if(NOT HLOLLI_WG_ANALYZER_ENABLE_ONNX)
  add_test(
    NAME cli.infer-note-events-requires-onnx
    COMMAND hlolli_wg_analyzer infer-note-events missing.wav
      --model missing.onnx --output missing.hwa-events)
  set_tests_properties(
    cli.infer-note-events-requires-onnx PROPERTIES
    WILL_FAIL TRUE
    TIMEOUT 30)
  add_test(
    NAME cli.separate-instruments-requires-onnx
    COMMAND hlolli_wg_analyzer separate-instruments missing.wav
      --model missing.onnx --output missing.hwa-events)
  set_tests_properties(
    cli.separate-instruments-requires-onnx PROPERTIES
    WILL_FAIL TRUE
    TIMEOUT 30)
  add_test(
    NAME cli.infer-stem-note-events-requires-onnx
    COMMAND hlolli_wg_analyzer infer-stem-note-events missing.hwa-events
      --model missing.onnx --output derived.hwa-events)
  set_tests_properties(
    cli.infer-stem-note-events-requires-onnx PROPERTIES
    WILL_FAIL TRUE
    TIMEOUT 30)
endif()

add_executable(
  hlolli_wg_analyzer_inference_onnx_tests
  inference_onnx_tests.c
  ../src/inference_onnx.c)
target_include_directories(
  hlolli_wg_analyzer_inference_onnx_tests PRIVATE . ../src)
target_compile_definitions(
  hlolli_wg_analyzer_inference_onnx_tests PRIVATE HWA_HAVE_ONNX_RUNTIME=1)
target_compile_features(
  hlolli_wg_analyzer_inference_onnx_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_inference_onnx_tests)
add_test(
  NAME unit.inference-onnx-capability
  COMMAND hlolli_wg_analyzer_inference_onnx_tests)
set_tests_properties(unit.inference-onnx-capability PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_basic_pitch_tests
  basic_pitch_tests.c)
target_include_directories(
  hlolli_wg_analyzer_basic_pitch_tests PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_basic_pitch_tests PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_basic_pitch_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_basic_pitch_tests)
add_test(
  NAME unit.basic-pitch-decoder
  COMMAND hlolli_wg_analyzer_basic_pitch_tests)
set_tests_properties(unit.basic-pitch-decoder PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_basic_pitch_provider_tests
  basic_pitch_provider_tests.c)
target_include_directories(
  hlolli_wg_analyzer_basic_pitch_provider_tests PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_basic_pitch_provider_tests PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_basic_pitch_provider_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_basic_pitch_provider_tests)
add_test(
  NAME unit.basic-pitch-provider
  COMMAND hlolli_wg_analyzer_basic_pitch_provider_tests)
set_tests_properties(unit.basic-pitch-provider PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_basic_pitch_audio_provider_tests
  basic_pitch_audio_provider_tests.c)
target_include_directories(
  hlolli_wg_analyzer_basic_pitch_audio_provider_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_basic_pitch_audio_provider_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_basic_pitch_audio_provider_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_basic_pitch_audio_provider_tests)
add_test(
  NAME unit.basic-pitch-audio-provider
  COMMAND hlolli_wg_analyzer_basic_pitch_audio_provider_tests)
set_tests_properties(unit.basic-pitch-audio-provider PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_stem_note_derivation_tests
  stem_note_derivation_tests.c)
target_include_directories(
  hlolli_wg_analyzer_stem_note_derivation_tests PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stem_note_derivation_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stem_note_derivation_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stem_note_derivation_tests)
add_test(
  NAME unit.stem-note-derivation
  COMMAND hlolli_wg_analyzer_stem_note_derivation_tests)
set_tests_properties(unit.stem-note-derivation PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_instrument_stem_provider_tests
  instrument_stem_provider_tests.c)
target_include_directories(
  hlolli_wg_analyzer_instrument_stem_provider_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_instrument_stem_provider_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_instrument_stem_provider_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_instrument_stem_provider_tests)
add_test(
  NAME unit.instrument-stem-provider
  COMMAND hlolli_wg_analyzer_instrument_stem_provider_tests)
set_tests_properties(unit.instrument-stem-provider PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_htdemucs_tests
  htdemucs_tests.c)
target_include_directories(
  hlolli_wg_analyzer_htdemucs_tests PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_htdemucs_tests PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_htdemucs_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_htdemucs_tests)
add_test(
  NAME unit.htdemucs-adapter
  COMMAND hlolli_wg_analyzer_htdemucs_tests)
set_tests_properties(unit.htdemucs-adapter PROPERTIES TIMEOUT 60)

if(HLOLLI_WG_ANALYZER_BASIC_PITCH_TEST_MODEL)
  add_executable(
    hlolli_wg_analyzer_basic_pitch_onnx_cli_tests
    basic_pitch_onnx_cli_tests.c)
  target_include_directories(
    hlolli_wg_analyzer_basic_pitch_onnx_cli_tests PRIVATE ../src ../include)
  target_link_libraries(
    hlolli_wg_analyzer_basic_pitch_onnx_cli_tests
    PRIVATE hlolli_wg_analyzer_core)
  target_compile_definitions(
    hlolli_wg_analyzer_basic_pitch_onnx_cli_tests PRIVATE
    "HWA_BASIC_PITCH_TEST_MODEL_SHA256=\"${HWA_BASIC_PITCH_REFERENCE_MODEL_SHA256}\"")
  target_compile_features(
    hlolli_wg_analyzer_basic_pitch_onnx_cli_tests PRIVATE c_std_11)
  hwa_set_warnings(hlolli_wg_analyzer_basic_pitch_onnx_cli_tests)
  add_test(
    NAME integration.basic-pitch-onnx
    COMMAND hlolli_wg_analyzer_basic_pitch_onnx_cli_tests
            $<TARGET_FILE:hlolli_wg_analyzer>
            "${HWA_BASIC_PITCH_TEST_MODEL_PATH}")
  set_tests_properties(
    integration.basic-pitch-onnx PROPERTIES
    LABELS "integration;onnx"
    REQUIRED_FILES "${HWA_BASIC_PITCH_TEST_MODEL_PATH}"
    RUN_SERIAL TRUE
    TIMEOUT 180)

  add_executable(
    hlolli_wg_analyzer_stem_note_onnx_cli_tests
    stem_note_onnx_cli_tests.c)
  target_include_directories(
    hlolli_wg_analyzer_stem_note_onnx_cli_tests PRIVATE ../src ../include)
  target_link_libraries(
    hlolli_wg_analyzer_stem_note_onnx_cli_tests
    PRIVATE hlolli_wg_analyzer_core)
  target_compile_definitions(
    hlolli_wg_analyzer_stem_note_onnx_cli_tests PRIVATE
    "HWA_BASIC_PITCH_TEST_MODEL_SHA256=\"${HWA_BASIC_PITCH_REFERENCE_MODEL_SHA256}\"")
  target_compile_features(
    hlolli_wg_analyzer_stem_note_onnx_cli_tests PRIVATE c_std_11)
  hwa_set_warnings(hlolli_wg_analyzer_stem_note_onnx_cli_tests)
  add_test(
    NAME integration.stem-note-onnx
    COMMAND hlolli_wg_analyzer_stem_note_onnx_cli_tests
            $<TARGET_FILE:hlolli_wg_analyzer>
            "${HWA_BASIC_PITCH_TEST_MODEL_PATH}")
  set_tests_properties(
    integration.stem-note-onnx PROPERTIES
    LABELS "integration;onnx"
    REQUIRED_FILES "${HWA_BASIC_PITCH_TEST_MODEL_PATH}"
    RUN_SERIAL TRUE
    TIMEOUT 240)
endif()

if(HLOLLI_WG_ANALYZER_HTDEMUCS_TEST_MODEL)
  add_executable(
    hlolli_wg_analyzer_htdemucs_onnx_cli_tests
    htdemucs_onnx_cli_tests.c)
  target_include_directories(
    hlolli_wg_analyzer_htdemucs_onnx_cli_tests PRIVATE ../src ../include)
  target_link_libraries(
    hlolli_wg_analyzer_htdemucs_onnx_cli_tests
    PRIVATE hlolli_wg_analyzer_core)
  target_compile_definitions(
    hlolli_wg_analyzer_htdemucs_onnx_cli_tests PRIVATE
    "HWA_HTDEMUCS_TEST_MODEL_SHA256=\"${HWA_HTDEMUCS_REFERENCE_MODEL_SHA256}\"")
  target_compile_features(
    hlolli_wg_analyzer_htdemucs_onnx_cli_tests PRIVATE c_std_11)
  hwa_set_warnings(hlolli_wg_analyzer_htdemucs_onnx_cli_tests)
  add_test(
    NAME integration.htdemucs-onnx
    COMMAND hlolli_wg_analyzer_htdemucs_onnx_cli_tests
            $<TARGET_FILE:hlolli_wg_analyzer>
            "${HWA_HTDEMUCS_TEST_MODEL_PATH}")
  set_tests_properties(
    integration.htdemucs-onnx PROPERTIES
    LABELS "integration;onnx"
    REQUIRED_FILES "${HWA_HTDEMUCS_TEST_MODEL_PATH}"
    RUN_SERIAL TRUE
    TIMEOUT 300)
endif()

add_executable(
  hlolli_wg_analyzer_event_analysis_tests
  event_analysis_tests.c)
target_include_directories(
  hlolli_wg_analyzer_event_analysis_tests PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_event_analysis_tests PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_event_analysis_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_event_analysis_tests)
add_test(
  NAME unit.event-analysis
  COMMAND hlolli_wg_analyzer_event_analysis_tests)
set_tests_properties(unit.event-analysis PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_event_analysis_cli_tests
  event_analysis_cli_tests.c)
target_include_directories(
  hlolli_wg_analyzer_event_analysis_cli_tests PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_event_analysis_cli_tests PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_event_analysis_cli_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_event_analysis_cli_tests)
add_test(
  NAME cli.analyze-events
  COMMAND hlolli_wg_analyzer_event_analysis_cli_tests
          $<TARGET_FILE:hlolli_wg_analyzer>)
set_tests_properties(cli.analyze-events PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_event_bundle_cli_tests
  event_bundle_cli_tests.c)
target_include_directories(
  hlolli_wg_analyzer_event_bundle_cli_tests PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_event_bundle_cli_tests PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_event_bundle_cli_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_event_bundle_cli_tests)
add_test(
  NAME cli.validate-event-bundle
  COMMAND hlolli_wg_analyzer_event_bundle_cli_tests
          $<TARGET_FILE:hlolli_wg_analyzer>)
set_tests_properties(cli.validate-event-bundle PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_event_csound_roundtrip_tests
  event_csound_roundtrip_tests.c)
target_include_directories(
  hlolli_wg_analyzer_event_csound_roundtrip_tests PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_event_csound_roundtrip_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_event_csound_roundtrip_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_event_csound_roundtrip_tests)
add_test(
  NAME unit.event-csound-score
  COMMAND hlolli_wg_analyzer_event_csound_roundtrip_tests)
set_tests_properties(unit.event-csound-score PROPERTIES TIMEOUT 30)
find_program(HWA_CSOUND_EXECUTABLE NAMES csound)
if(HWA_CSOUND_EXECUTABLE)
  add_test(
    NAME integration.event-csound-roundtrip
    COMMAND hlolli_wg_analyzer_event_csound_roundtrip_tests
            "${HWA_CSOUND_EXECUTABLE}")
  set_tests_properties(integration.event-csound-roundtrip PROPERTIES TIMEOUT 30)
endif()

if(NOT MSVC)
  target_link_libraries(hlolli_wg_analyzer_cli_tests PRIVATE m)
endif()

add_executable(
  hlolli_wg_analyzer_json_output_tests
  json_output_tests.c
  ../src/output.c)
target_include_directories(
  hlolli_wg_analyzer_json_output_tests
  PRIVATE ../src)
target_compile_features(hlolli_wg_analyzer_json_output_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_json_output_tests)
add_test(NAME unit.json-output COMMAND hlolli_wg_analyzer_json_output_tests)
set_tests_properties(unit.json-output PROPERTIES TIMEOUT 15)

add_executable(
  hlolli_wg_analyzer_file_output_tests
  file_output_tests.c
  ../src/file_output.c)
target_include_directories(
  hlolli_wg_analyzer_file_output_tests
  PRIVATE ../src)
target_compile_features(
  hlolli_wg_analyzer_file_output_tests PRIVATE c_std_11)
target_compile_definitions(
  hlolli_wg_analyzer_file_output_tests PRIVATE HWA_FILE_OUTPUT_TESTING=1)
hwa_set_warnings(hlolli_wg_analyzer_file_output_tests)
add_test(NAME unit.file-output COMMAND hlolli_wg_analyzer_file_output_tests)
set_tests_properties(unit.file-output PROPERTIES TIMEOUT 15)

add_executable(hlolli_wg_analyzer_sha256_tests sha256_tests.c)
target_include_directories(
  hlolli_wg_analyzer_sha256_tests
  PRIVATE ../src)
target_link_libraries(
  hlolli_wg_analyzer_sha256_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(hlolli_wg_analyzer_sha256_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_sha256_tests)
add_test(NAME unit.sha256 COMMAND hlolli_wg_analyzer_sha256_tests)
set_tests_properties(unit.sha256 PROPERTIES TIMEOUT 15)

add_executable(
  hlolli_wg_analyzer_numeric_locale_tests
  numeric_locale_tests.c)
target_include_directories(
  hlolli_wg_analyzer_numeric_locale_tests
  PRIVATE ../src)
target_link_libraries(
  hlolli_wg_analyzer_numeric_locale_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_numeric_locale_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_numeric_locale_tests)
add_test(
  NAME unit.numeric-locale
  COMMAND hlolli_wg_analyzer_numeric_locale_tests)
set_tests_properties(
  unit.numeric-locale PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../src/score_manifest.c")
  add_executable(hlolli_wg_analyzer_score_manifest_tests
    score_manifest_tests.c)
  target_include_directories(
    hlolli_wg_analyzer_score_manifest_tests
    PRIVATE ../src ../include)
  target_link_libraries(
    hlolli_wg_analyzer_score_manifest_tests
    PRIVATE hlolli_wg_analyzer_core)
  target_compile_features(
    hlolli_wg_analyzer_score_manifest_tests PRIVATE c_std_11)
  hwa_set_warnings(hlolli_wg_analyzer_score_manifest_tests)
  add_test(
    NAME unit.score-manifest
    COMMAND hlolli_wg_analyzer_score_manifest_tests)
  set_tests_properties(unit.score-manifest PROPERTIES TIMEOUT 30)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../src/alignment.c")
  add_executable(hlolli_wg_analyzer_alignment_tests alignment_tests.c)
  target_include_directories(
    hlolli_wg_analyzer_alignment_tests
    PRIVATE ../src ../include)
  target_link_libraries(
    hlolli_wg_analyzer_alignment_tests
    PRIVATE hlolli_wg_analyzer_core)
  target_compile_features(
    hlolli_wg_analyzer_alignment_tests PRIVATE c_std_11)
  hwa_set_warnings(hlolli_wg_analyzer_alignment_tests)
  add_test(NAME unit.alignment COMMAND hlolli_wg_analyzer_alignment_tests)
  set_tests_properties(unit.alignment PROPERTIES TIMEOUT 30)
endif()

add_executable(
  hlolli_wg_analyzer_segmentation_tests
  segmentation_tests.c)
target_include_directories(
  hlolli_wg_analyzer_segmentation_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_segmentation_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_segmentation_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_segmentation_tests)
add_test(
  NAME unit.segmentation
  COMMAND hlolli_wg_analyzer_segmentation_tests)
set_tests_properties(unit.segmentation PROPERTIES TIMEOUT 60)

add_executable(
  hlolli_wg_analyzer_stage3_persistence_tests
  stage3_persistence_tests.c)
target_include_directories(
  hlolli_wg_analyzer_stage3_persistence_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage3_persistence_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage3_persistence_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage3_persistence_tests)
add_test(
  NAME unit.stage3-persistence
  COMMAND hlolli_wg_analyzer_stage3_persistence_tests)
set_tests_properties(unit.stage3-persistence PROPERTIES TIMEOUT 60)

add_executable(
  hlolli_wg_analyzer_stage4_item_file_tests
  stage4_item_file_tests.c)
target_include_directories(
  hlolli_wg_analyzer_stage4_item_file_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage4_item_file_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage4_item_file_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage4_item_file_tests)
add_test(
  NAME unit.stage4-item-file
  COMMAND hlolli_wg_analyzer_stage4_item_file_tests)
set_tests_properties(unit.stage4-item-file PROPERTIES TIMEOUT 60)

add_executable(
  hlolli_wg_analyzer_measure_engine_tests
  measure_engine_tests.c)
target_include_directories(
  hlolli_wg_analyzer_measure_engine_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_measure_engine_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_measure_engine_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_measure_engine_tests)
add_test(
  NAME unit.measure-engine
  COMMAND hlolli_wg_analyzer_measure_engine_tests)
set_tests_properties(unit.measure-engine PROPERTIES TIMEOUT 120)

add_executable(
  hlolli_wg_analyzer_stage4_persistence_tests
  stage4_persistence_tests.c)
target_include_directories(
  hlolli_wg_analyzer_stage4_persistence_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage4_persistence_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage4_persistence_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage4_persistence_tests)
add_test(
  NAME unit.stage4-persistence
  COMMAND hlolli_wg_analyzer_stage4_persistence_tests)
set_tests_properties(unit.stage4-persistence PROPERTIES TIMEOUT 120)

add_executable(
  hlolli_wg_analyzer_stage4_report_tests
  stage4_report_tests.c
  ../src/measure_report.c
  ../src/output.c)
target_include_directories(
  hlolli_wg_analyzer_stage4_report_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage4_report_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage4_report_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage4_report_tests)
add_test(
  NAME unit.stage4-report
  COMMAND hlolli_wg_analyzer_stage4_report_tests)
set_tests_properties(unit.stage4-report PROPERTIES TIMEOUT 60)

add_executable(
  hlolli_wg_analyzer_stage4_cli_tests
  stage4_cli_tests.c)
target_include_directories(
  hlolli_wg_analyzer_stage4_cli_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage4_cli_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage4_cli_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage4_cli_tests)
add_test(
  NAME cli.stage4-measure-compare
  COMMAND
    hlolli_wg_analyzer_stage4_cli_tests
    $<TARGET_FILE:hlolli_wg_analyzer>)
set_tests_properties(cli.stage4-measure-compare PROPERTIES TIMEOUT 180)

add_executable(
  hlolli_wg_analyzer_physical_check_tests
  physical_check_tests.c)
target_include_directories(
  hlolli_wg_analyzer_physical_check_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_physical_check_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_physical_check_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_physical_check_tests)
add_test(
  NAME unit.physical-check
  COMMAND hlolli_wg_analyzer_physical_check_tests)
set_tests_properties(unit.physical-check PROPERTIES TIMEOUT 180)

add_executable(
  hlolli_wg_analyzer_stage5_persistence_tests
  stage5_persistence_tests.c)
target_include_directories(
  hlolli_wg_analyzer_stage5_persistence_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage5_persistence_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage5_persistence_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage5_persistence_tests)
add_test(
  NAME unit.stage5-persistence
  COMMAND hlolli_wg_analyzer_stage5_persistence_tests)
set_tests_properties(unit.stage5-persistence PROPERTIES TIMEOUT 180)

add_executable(
  hlolli_wg_analyzer_stage5_report_tests
  stage5_report_tests.c
  ../src/physical_report.c
  ../src/output.c)
target_include_directories(
  hlolli_wg_analyzer_stage5_report_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage5_report_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage5_report_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage5_report_tests)
add_test(
  NAME unit.stage5-report
  COMMAND hlolli_wg_analyzer_stage5_report_tests)
set_tests_properties(unit.stage5-report PROPERTIES TIMEOUT 90)

add_executable(
  hlolli_wg_analyzer_stage5_cli_tests
  stage5_cli_tests.c)
target_include_directories(
  hlolli_wg_analyzer_stage5_cli_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage5_cli_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage5_cli_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage5_cli_tests)
add_test(
  NAME cli.stage5-physical-check
  COMMAND
    hlolli_wg_analyzer_stage5_cli_tests
    $<TARGET_FILE:hlolli_wg_analyzer>)
set_tests_properties(cli.stage5-physical-check PROPERTIES TIMEOUT 300)

add_executable(
  hlolli_wg_analyzer_production_tests
  production_tests.c)
target_include_directories(
  hlolli_wg_analyzer_production_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_production_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_production_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_production_tests)
add_test(
  NAME unit.production
  COMMAND hlolli_wg_analyzer_production_tests)
set_tests_properties(unit.production PROPERTIES TIMEOUT 300)

add_executable(
  hlolli_wg_analyzer_stage6_persistence_tests
  stage6_persistence_tests.c)
target_include_directories(
  hlolli_wg_analyzer_stage6_persistence_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage6_persistence_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage6_persistence_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage6_persistence_tests)
add_test(
  NAME unit.stage6-persistence
  COMMAND hlolli_wg_analyzer_stage6_persistence_tests)
set_tests_properties(unit.stage6-persistence PROPERTIES TIMEOUT 180)

add_executable(
  hlolli_wg_analyzer_stage6_report_tests
  stage6_report_tests.c
  ../src/production_report.c
  ../src/output.c)
target_include_directories(
  hlolli_wg_analyzer_stage6_report_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage6_report_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage6_report_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage6_report_tests)
add_test(
  NAME unit.stage6-report
  COMMAND hlolli_wg_analyzer_stage6_report_tests)
set_tests_properties(unit.stage6-report PROPERTIES TIMEOUT 90)

add_executable(
  hlolli_wg_analyzer_stage6_cli_tests
  stage6_cli_tests.c)
target_include_directories(
  hlolli_wg_analyzer_stage6_cli_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage6_cli_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage6_cli_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage6_cli_tests)
add_test(
  NAME cli.stage6-account-production
  COMMAND
    hlolli_wg_analyzer_stage6_cli_tests
    $<TARGET_FILE:hlolli_wg_analyzer>)
set_tests_properties(cli.stage6-account-production PROPERTIES TIMEOUT 300)

  add_executable(
    hlolli_wg_analyzer_run_tests
    run_tests.c)
  target_include_directories(
    hlolli_wg_analyzer_run_tests
    PRIVATE ../src ../include)
  target_link_libraries(
    hlolli_wg_analyzer_run_tests
    PRIVATE hlolli_wg_analyzer_core)
  target_compile_features(
    hlolli_wg_analyzer_run_tests PRIVATE c_std_11)
  hwa_set_warnings(hlolli_wg_analyzer_run_tests)
  add_test(
    NAME unit.run
    COMMAND hlolli_wg_analyzer_run_tests)
  set_tests_properties(unit.run PROPERTIES TIMEOUT 300)

  add_executable(
    hlolli_wg_analyzer_stage7_persistence_tests
    stage7_persistence_tests.c)
  target_include_directories(
    hlolli_wg_analyzer_stage7_persistence_tests
    PRIVATE ../src ../include)
  target_link_libraries(
    hlolli_wg_analyzer_stage7_persistence_tests
    PRIVATE hlolli_wg_analyzer_core)
  target_compile_features(
    hlolli_wg_analyzer_stage7_persistence_tests PRIVATE c_std_11)
  hwa_set_warnings(hlolli_wg_analyzer_stage7_persistence_tests)
  add_test(
    NAME unit.stage7-persistence
    COMMAND hlolli_wg_analyzer_stage7_persistence_tests)
  set_tests_properties(unit.stage7-persistence PROPERTIES TIMEOUT 180)

  add_executable(
    hlolli_wg_analyzer_stage7_report_tests
    stage7_report_tests.c
    ../src/run_report.c
    ../src/output.c)
  target_include_directories(
    hlolli_wg_analyzer_stage7_report_tests
    PRIVATE ../src ../include)
  target_link_libraries(
    hlolli_wg_analyzer_stage7_report_tests
    PRIVATE hlolli_wg_analyzer_core)
  target_compile_features(
    hlolli_wg_analyzer_stage7_report_tests PRIVATE c_std_11)
  hwa_set_warnings(hlolli_wg_analyzer_stage7_report_tests)
  add_test(
    NAME unit.stage7-report
    COMMAND hlolli_wg_analyzer_stage7_report_tests)
  set_tests_properties(unit.stage7-report PROPERTIES TIMEOUT 90)

  add_executable(
    hlolli_wg_analyzer_stage7_cli_tests
    stage7_cli_tests.c)
  target_include_directories(
    hlolli_wg_analyzer_stage7_cli_tests
    PRIVATE ../src ../include)
  target_link_libraries(
    hlolli_wg_analyzer_stage7_cli_tests
    PRIVATE hlolli_wg_analyzer_core)
  target_compile_features(
    hlolli_wg_analyzer_stage7_cli_tests PRIVATE c_std_11)
  hwa_set_warnings(hlolli_wg_analyzer_stage7_cli_tests)
  add_test(
    NAME cli.stage7-analyze-run
    COMMAND
      hlolli_wg_analyzer_stage7_cli_tests
      $<TARGET_FILE:hlolli_wg_analyzer>)
  set_tests_properties(cli.stage7-analyze-run PROPERTIES TIMEOUT 300)

add_executable(
  hlolli_wg_analyzer_experiment_tests
  experiment_tests.c)
target_include_directories(
  hlolli_wg_analyzer_experiment_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_experiment_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_experiment_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_experiment_tests)
add_test(
  NAME unit.experiment
  COMMAND hlolli_wg_analyzer_experiment_tests)
set_tests_properties(unit.experiment PROPERTIES TIMEOUT 300)

add_executable(
  hlolli_wg_analyzer_stage8_renderer_ok
  stage8_renderer_helper.c)
target_compile_features(
  hlolli_wg_analyzer_stage8_renderer_ok PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage8_renderer_ok)

add_executable(
  hlolli_wg_analyzer_stage8_renderer_fail
  stage8_renderer_helper.c)
target_compile_definitions(
  hlolli_wg_analyzer_stage8_renderer_fail
  PRIVATE HWA_STAGE8_RENDERER_MODE=1)
target_compile_features(
  hlolli_wg_analyzer_stage8_renderer_fail PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage8_renderer_fail)

add_executable(
  hlolli_wg_analyzer_stage8_renderer_slow
  stage8_renderer_helper.c)
target_compile_definitions(
  hlolli_wg_analyzer_stage8_renderer_slow
  PRIVATE HWA_STAGE8_RENDERER_MODE=2)
target_compile_features(
  hlolli_wg_analyzer_stage8_renderer_slow PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage8_renderer_slow)

add_executable(
  hlolli_wg_analyzer_stage8_renderer_large
  stage8_renderer_helper.c)
target_compile_definitions(
  hlolli_wg_analyzer_stage8_renderer_large
  PRIVATE HWA_STAGE8_RENDERER_MODE=3)
target_compile_features(
  hlolli_wg_analyzer_stage8_renderer_large PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage8_renderer_large)

add_executable(
  hlolli_wg_analyzer_stage8_renderer_extra
  stage8_renderer_helper.c)
target_compile_definitions(
  hlolli_wg_analyzer_stage8_renderer_extra
  PRIVATE HWA_STAGE8_RENDERER_MODE=4)
target_compile_features(
  hlolli_wg_analyzer_stage8_renderer_extra PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage8_renderer_extra)

add_executable(
  hlolli_wg_analyzer_stage8_renderer_worker
  stage8_renderer_helper.c)
target_compile_definitions(
  hlolli_wg_analyzer_stage8_renderer_worker
  PRIVATE HWA_STAGE8_RENDERER_MODE=5)
target_compile_features(
  hlolli_wg_analyzer_stage8_renderer_worker PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage8_renderer_worker)

add_executable(
  hlolli_wg_analyzer_stage8_renderer_quick
  stage8_renderer_helper.c)
target_compile_definitions(
  hlolli_wg_analyzer_stage8_renderer_quick
  PRIVATE HWA_STAGE8_RENDERER_MODE=6)
target_compile_features(
  hlolli_wg_analyzer_stage8_renderer_quick PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage8_renderer_quick)

add_executable(
  hlolli_wg_analyzer_stage8_renderer_churn
  stage8_renderer_helper.c)
target_compile_definitions(
  hlolli_wg_analyzer_stage8_renderer_churn
  PRIVATE HWA_STAGE8_RENDERER_MODE=7)
target_compile_features(
  hlolli_wg_analyzer_stage8_renderer_churn PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage8_renderer_churn)

add_executable(
  hlolli_wg_analyzer_stage8_cli_tests
  stage8_cli_tests.c)
target_include_directories(
  hlolli_wg_analyzer_stage8_cli_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage8_cli_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage8_cli_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage8_cli_tests)
add_test(
  NAME cli.stage8-experiment
  COMMAND
    hlolli_wg_analyzer_stage8_cli_tests
    $<TARGET_FILE:hlolli_wg_analyzer>
    $<TARGET_FILE:hlolli_wg_analyzer_stage8_renderer_ok>
    $<TARGET_FILE:hlolli_wg_analyzer_stage8_renderer_fail>
    $<TARGET_FILE:hlolli_wg_analyzer_stage8_renderer_slow>
    $<TARGET_FILE:hlolli_wg_analyzer_stage8_renderer_large>
    $<TARGET_FILE:hlolli_wg_analyzer_stage8_renderer_extra>
    $<TARGET_FILE:hlolli_wg_analyzer_stage8_renderer_worker>
    $<TARGET_FILE:hlolli_wg_analyzer_stage8_renderer_quick>
    $<TARGET_FILE:hlolli_wg_analyzer_stage8_renderer_churn>)
set_tests_properties(cli.stage8-experiment PROPERTIES TIMEOUT 300)

add_executable(
  hlolli_wg_analyzer_stage8_persistence_tests
  stage8_persistence_tests.c)
target_include_directories(
  hlolli_wg_analyzer_stage8_persistence_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage8_persistence_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage8_persistence_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage8_persistence_tests)
add_test(
  NAME unit.stage8-persistence
  COMMAND hlolli_wg_analyzer_stage8_persistence_tests)
set_tests_properties(unit.stage8-persistence PROPERTIES TIMEOUT 180)

add_executable(
  hlolli_wg_analyzer_stage8_report_tests
  stage8_report_tests.c
  ../src/experiment_report.c
  ../src/output.c)
target_include_directories(
  hlolli_wg_analyzer_stage8_report_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage8_report_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage8_report_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage8_report_tests)
add_test(
  NAME unit.stage8-report
  COMMAND hlolli_wg_analyzer_stage8_report_tests)
set_tests_properties(unit.stage8-report PROPERTIES TIMEOUT 90)

add_executable(
  hlolli_wg_analyzer_gap_report_tests
  gap_report_tests.c)
target_include_directories(
  hlolli_wg_analyzer_gap_report_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_gap_report_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_gap_report_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_gap_report_tests)
add_test(
  NAME unit.gap-report
  COMMAND hlolli_wg_analyzer_gap_report_tests)
set_tests_properties(unit.gap-report PROPERTIES TIMEOUT 300)

add_executable(
  hlolli_wg_analyzer_stage9_persistence_tests
  stage9_persistence_tests.c)
target_include_directories(
  hlolli_wg_analyzer_stage9_persistence_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage9_persistence_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage9_persistence_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage9_persistence_tests)
add_test(
  NAME unit.stage9-persistence
  COMMAND hlolli_wg_analyzer_stage9_persistence_tests)
set_tests_properties(unit.stage9-persistence PROPERTIES TIMEOUT 180)

add_executable(
  hlolli_wg_analyzer_stage9_output_tests
  stage9_output_tests.c
  ../src/output.c)
target_include_directories(
  hlolli_wg_analyzer_stage9_output_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage9_output_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage9_output_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage9_output_tests)
add_test(
  NAME unit.stage9-output
  COMMAND hlolli_wg_analyzer_stage9_output_tests)
set_tests_properties(unit.stage9-output PROPERTIES TIMEOUT 90)

add_executable(
  hlolli_wg_analyzer_stage9_clip_tests
  stage9_clip_tests.c)
target_include_directories(
  hlolli_wg_analyzer_stage9_clip_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage9_clip_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage9_clip_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage9_clip_tests)
add_test(
  NAME unit.stage9-clip
  COMMAND hlolli_wg_analyzer_stage9_clip_tests)
set_tests_properties(unit.stage9-clip PROPERTIES TIMEOUT 180)

add_executable(
  hlolli_wg_analyzer_stage9_cli_tests
  stage9_cli_tests.c)
target_include_directories(
  hlolli_wg_analyzer_stage9_cli_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage9_cli_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage9_cli_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage9_cli_tests)
add_test(
  NAME cli.stage9-gap-report
  COMMAND
    hlolli_wg_analyzer_stage9_cli_tests
    $<TARGET_FILE:hlolli_wg_analyzer>)
set_tests_properties(cli.stage9-gap-report PROPERTIES TIMEOUT 300)

add_executable(
  hlolli_wg_analyzer_stage3_report_tests
  stage3_report_tests.c
  ../src/item_report.c
  ../src/output.c)
target_include_directories(
  hlolli_wg_analyzer_stage3_report_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_stage3_report_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_stage3_report_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage3_report_tests)
add_test(
  NAME unit.stage3-report
  COMMAND hlolli_wg_analyzer_stage3_report_tests)
set_tests_properties(unit.stage3-report PROPERTIES TIMEOUT 30)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../src/alignment_file.c")
  add_executable(
    hlolli_wg_analyzer_alignment_file_tests
    alignment_file_tests.c)
  target_include_directories(
    hlolli_wg_analyzer_alignment_file_tests
    PRIVATE ../src ../include)
  target_link_libraries(
    hlolli_wg_analyzer_alignment_file_tests
    PRIVATE hlolli_wg_analyzer_core)
  target_compile_features(
    hlolli_wg_analyzer_alignment_file_tests PRIVATE c_std_11)
  hwa_set_warnings(hlolli_wg_analyzer_alignment_file_tests)
  add_test(
    NAME unit.alignment-file
    COMMAND hlolli_wg_analyzer_alignment_file_tests)
  set_tests_properties(unit.alignment-file PROPERTIES TIMEOUT 30)
endif()

add_executable(hlolli_wg_analyzer_analysis_api_tests analysis_api_tests.c)
target_include_directories(
  hlolli_wg_analyzer_analysis_api_tests
  PRIVATE ../include)
target_link_libraries(
  hlolli_wg_analyzer_analysis_api_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_analysis_api_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_analysis_api_tests)
add_test(
  NAME unit.analysis-api
  COMMAND hlolli_wg_analyzer_analysis_api_tests)
set_tests_properties(unit.analysis-api PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_isolated_note_tests
  isolated_note_tests.c)
target_include_directories(
  hlolli_wg_analyzer_isolated_note_tests
  PRIVATE ../include)
target_link_libraries(
  hlolli_wg_analyzer_isolated_note_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_isolated_note_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_isolated_note_tests)
add_test(
  NAME unit.isolated-note
  COMMAND hlolli_wg_analyzer_isolated_note_tests)
set_tests_properties(unit.isolated-note PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_harmonic_decay_tests
  harmonic_decay_tests.c)
target_include_directories(
  hlolli_wg_analyzer_harmonic_decay_tests PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_harmonic_decay_tests PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_harmonic_decay_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_harmonic_decay_tests)
add_test(
  NAME unit.harmonic-decay
  COMMAND hlolli_wg_analyzer_harmonic_decay_tests)
set_tests_properties(unit.harmonic-decay PROPERTIES TIMEOUT 60)

add_executable(
  hlolli_wg_analyzer_report_tests
  report_tests.c
  ../src/report.c
  ../src/output.c)
target_include_directories(
  hlolli_wg_analyzer_report_tests
  PRIVATE ../src ../include)
target_link_libraries(
  hlolli_wg_analyzer_report_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_report_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_report_tests)
add_test(NAME unit.report COMMAND hlolli_wg_analyzer_report_tests)
set_tests_properties(unit.report PROPERTIES TIMEOUT 30)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dsp_tests.c")
  add_executable(
    hlolli_wg_analyzer_dsp_tests
    dsp_tests.c
    ../src/dsp.c)
  target_include_directories(
    hlolli_wg_analyzer_dsp_tests
    PRIVATE ../src)
  target_compile_features(hlolli_wg_analyzer_dsp_tests PRIVATE c_std_11)
  hwa_set_warnings(hlolli_wg_analyzer_dsp_tests)
  if(NOT MSVC)
    target_link_libraries(hlolli_wg_analyzer_dsp_tests PRIVATE m)
  endif()
  add_test(NAME unit.dsp COMMAND hlolli_wg_analyzer_dsp_tests)
  set_tests_properties(unit.dsp PROPERTIES TIMEOUT 30)
endif()

add_executable(
  hlolli_wg_analyzer_portability_tests
  portability_tests.c)
target_include_directories(
  hlolli_wg_analyzer_portability_tests
  PRIVATE ../include)
target_link_libraries(
  hlolli_wg_analyzer_portability_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_portability_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_portability_tests)
add_test(
  NAME unit.portability
  COMMAND hlolli_wg_analyzer_portability_tests)
set_tests_properties(unit.portability PROPERTIES TIMEOUT 30)

add_executable(
  hlolli_wg_analyzer_public_client_tests
  public_client_tests.c)
target_link_libraries(
  hlolli_wg_analyzer_public_client_tests
  PRIVATE hlolli_wg_analyzer_core)
target_compile_features(
  hlolli_wg_analyzer_public_client_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_public_client_tests)
add_test(
  NAME unit.public-client
  COMMAND hlolli_wg_analyzer_public_client_tests)
set_tests_properties(unit.public-client PROPERTIES TIMEOUT 30)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/features_tests.c")
  add_executable(hlolli_wg_analyzer_features_tests features_tests.c)
  target_include_directories(
    hlolli_wg_analyzer_features_tests
    PRIVATE ../src ../include)
  target_link_libraries(
    hlolli_wg_analyzer_features_tests
    PRIVATE hlolli_wg_analyzer_core)
  target_compile_features(hlolli_wg_analyzer_features_tests PRIVATE c_std_11)
  hwa_set_warnings(hlolli_wg_analyzer_features_tests)
  add_test(NAME unit.features COMMAND hlolli_wg_analyzer_features_tests)
  set_tests_properties(unit.features PROPERTIES TIMEOUT 180)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/wav_reader_stage1_tests.c")
  add_executable(
    hlolli_wg_analyzer_wav_reader_stage1_tests
    wav_reader_stage1_tests.c
    ../src/wav_reader.c
    ../src/wav_reader_file.c)
  target_include_directories(
    hlolli_wg_analyzer_wav_reader_stage1_tests
    PRIVATE ../src ../include)
  target_compile_features(
    hlolli_wg_analyzer_wav_reader_stage1_tests PRIVATE c_std_11)
  hwa_set_warnings(hlolli_wg_analyzer_wav_reader_stage1_tests)
  if(NOT MSVC)
    target_link_libraries(
      hlolli_wg_analyzer_wav_reader_stage1_tests PRIVATE m)
  endif()
  add_test(
    NAME unit.wav-reader-stage1
    COMMAND hlolli_wg_analyzer_wav_reader_stage1_tests)
  set_tests_properties(unit.wav-reader-stage1 PROPERTIES TIMEOUT 30)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stage1_cli_tests.c")
  add_executable(
    hlolli_wg_analyzer_stage1_cli_tests
    stage1_cli_tests.c)
  target_compile_features(
    hlolli_wg_analyzer_stage1_cli_tests PRIVATE c_std_11)
  hwa_set_warnings(hlolli_wg_analyzer_stage1_cli_tests)
  if(NOT MSVC)
    target_link_libraries(hlolli_wg_analyzer_stage1_cli_tests PRIVATE m)
  endif()

  set(hlolli_wg_analyzer_stage1_cli_cases
    stage1-schema
    stage1-channels
    stage1-limits
    stage1-frames-export
    stage1-spectrogram-export
    stage1-output-safety
    stage1-block-invariance)
  if(NOT WIN32)
    list(
      APPEND hlolli_wg_analyzer_stage1_cli_cases
      stage1-stdin
      stage1-replace-write-failure)
  endif()
  foreach(case_name IN LISTS hlolli_wg_analyzer_stage1_cli_cases)
    add_test(
      NAME cli.${case_name}
      COMMAND
        hlolli_wg_analyzer_stage1_cli_tests
        $<TARGET_FILE:hlolli_wg_analyzer>
        ${case_name})
    set_tests_properties(cli.${case_name} PROPERTIES TIMEOUT 30)
  endforeach()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stage2_cli_tests.c")
  add_executable(
    hlolli_wg_analyzer_stage2_cli_tests
    stage2_cli_tests.c)
  target_compile_features(
    hlolli_wg_analyzer_stage2_cli_tests PRIVATE c_std_11)
  hwa_set_warnings(hlolli_wg_analyzer_stage2_cli_tests)
  if(NOT MSVC)
    target_link_libraries(hlolli_wg_analyzer_stage2_cli_tests PRIVATE m)
  endif()

  set(hlolli_wg_analyzer_stage2_cli_cases
    stage2-summary-line-endings
    stage2-audio
    stage2-score
    stage2-json
    stage2-amend
    stage2-block-invariance
    stage2-limits
    stage2-output-safety)
  if(NOT WIN32)
    list(
      APPEND hlolli_wg_analyzer_stage2_cli_cases
      stage2-output-failure)
  endif()
  foreach(case_name IN LISTS hlolli_wg_analyzer_stage2_cli_cases)
    add_test(
      NAME cli.${case_name}
      COMMAND
        hlolli_wg_analyzer_stage2_cli_tests
        $<TARGET_FILE:hlolli_wg_analyzer>
        ${case_name})
    set_tests_properties(cli.${case_name} PROPERTIES TIMEOUT 60)
  endforeach()
endif()

add_executable(
  hlolli_wg_analyzer_stage3_cli_tests
  stage3_cli_tests.c)
target_compile_features(
  hlolli_wg_analyzer_stage3_cli_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_stage3_cli_tests)
if(NOT MSVC)
  target_link_libraries(hlolli_wg_analyzer_stage3_cli_tests PRIVATE m)
endif()

set(hlolli_wg_analyzer_stage3_cli_cases
  stage3-basic
  stage3-json-labels
  stage3-rejections
  stage3-limits-output
  stage3-amend
  stage3-output-modes-blocks
  stage3-explicit-audio)
foreach(case_name IN LISTS hlolli_wg_analyzer_stage3_cli_cases)
  add_test(
    NAME cli.${case_name}
    COMMAND
      hlolli_wg_analyzer_stage3_cli_tests
      $<TARGET_FILE:hlolli_wg_analyzer>
      ${case_name})
  set_tests_properties(cli.${case_name} PROPERTIES TIMEOUT 120)
endforeach()
set(
  hlolli_wg_analyzer_cli_cases
  mono-text
  mono-json
  harmonic-decay-json
  stereo-json
  supported-formats
  compare-unequal-json
  reject-corrupt
  reject-truncated
  reject-unsupported
  reject-nonfinite-float
  reject-bad-extensible-size
  read-only)

if(NOT WIN32)
  list(
    APPEND hlolli_wg_analyzer_cli_cases
    invalid-utf8-path-json
    sparse-large-riff
    reject-fifo
    output-failure)
endif()

foreach(case_name IN LISTS hlolli_wg_analyzer_cli_cases)
  add_test(
    NAME cli.${case_name}
    COMMAND
      hlolli_wg_analyzer_cli_tests
      $<TARGET_FILE:hlolli_wg_analyzer>
      ${case_name})
  set_tests_properties(cli.${case_name} PROPERTIES TIMEOUT 15)
  if(case_name STREQUAL "invalid-utf8-path-json" OR
     case_name STREQUAL "sparse-large-riff")
    set_tests_properties(cli.${case_name} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endforeach()

find_program(HWA_PYTHON3_EXECUTABLE NAMES python3)
if(HWA_PYTHON3_EXECUTABLE)
  add_test(
    NAME cli.isolated-note
    COMMAND
      ${HWA_PYTHON3_EXECUTABLE}
      -B
      ${CMAKE_CURRENT_SOURCE_DIR}/test_isolated_note_cli.py
      --analyzer=$<TARGET_FILE:hlolli_wg_analyzer>)
  set_tests_properties(cli.isolated-note PROPERTIES TIMEOUT 60)

  add_test(
    NAME unit.bounded-pitch
    COMMAND
      ${HWA_PYTHON3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test_bounded_pitch.py)
  set_tests_properties(unit.bounded-pitch PROPERTIES TIMEOUT 30)

  add_test(
    NAME unit.instrument-fit
    COMMAND
      ${HWA_PYTHON3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test_instrument_fit.py)
  set_tests_properties(unit.instrument-fit PROPERTIES TIMEOUT 30)

  if(NOT WIN32)
    add_test(
      NAME unit.viola-fit-adapter
      COMMAND
        ${HWA_PYTHON3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_viola_fit_adapter.py)
    set_tests_properties(unit.viola-fit-adapter PROPERTIES TIMEOUT 60)
  endif()

  add_test(
    NAME unit.viola-recording-session
    COMMAND
      ${HWA_PYTHON3_EXECUTABLE}
      -B
      ${CMAKE_CURRENT_SOURCE_DIR}/test_viola_recording_session.py)
  set_tests_properties(unit.viola-recording-session PROPERTIES TIMEOUT 60)

  add_test(
    NAME unit.cello-fit-adapter
    COMMAND
      ${HWA_PYTHON3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/test_cello_fit_adapter.py)
  set_tests_properties(unit.cello-fit-adapter PROPERTIES TIMEOUT 30)

  if(NOT WIN32)
    add_test(
      NAME unit.double-bass-manifest
      COMMAND
        ${HWA_PYTHON3_EXECUTABLE}
        -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_double_bass_manifest.py)
    set_tests_properties(unit.double-bass-manifest PROPERTIES TIMEOUT 60)

    add_test(
      NAME unit.double-bass-recording-session
      COMMAND
        ${HWA_PYTHON3_EXECUTABLE}
        -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_double_bass_recording_session.py)
    set_tests_properties(
      unit.double-bass-recording-session PROPERTIES TIMEOUT 60)

    add_test(
      NAME unit.double-bass-adapter
      COMMAND
        ${HWA_PYTHON3_EXECUTABLE}
        -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_double_bass_adapter.py
        --analyzer=$<TARGET_FILE:hlolli_wg_analyzer>)
    set_tests_properties(unit.double-bass-adapter PROPERTIES TIMEOUT 120)

    add_test(
      NAME unit.double-bass-physical-dynamics
      COMMAND
        ${HWA_PYTHON3_EXECUTABLE}
        -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_double_bass_physical_dynamics.py)
    set_tests_properties(
      unit.double-bass-physical-dynamics PROPERTIES TIMEOUT 60)
  endif()
endif()
