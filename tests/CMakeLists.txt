add_executable(hlolli_wg_analyzer_cli_tests cli_tests.c)
target_compile_features(hlolli_wg_analyzer_cli_tests PRIVATE c_std_11)
hwa_set_warnings(hlolli_wg_analyzer_cli_tests)

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
    $<TARGET_FILE:hlolli_wg_analyzer_stage8_renderer_quick>)
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
