add_executable(hwa_benchmark
    benchmark.c
    ${PROJECT_SOURCE_DIR}/src/output.c
    ${PROJECT_SOURCE_DIR}/src/report.c)
target_include_directories(hwa_benchmark PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(hwa_benchmark PRIVATE hlolli_wg_analyzer_core)
if(NOT MSVC)
  target_link_libraries(hwa_benchmark PRIVATE m)
endif()
hwa_set_warnings(hwa_benchmark)

if(BUILD_TESTING)
  add_test(NAME benchmark.smoke
      COMMAND hwa_benchmark --samples 1 --minimum-ms 1)
  set_tests_properties(benchmark.smoke PROPERTIES
      TIMEOUT 120
      LABELS "benchmark;smoke")

  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME benchmark.comparator-self-test
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/compare.py --self-test)
    set_tests_properties(benchmark.comparator-self-test PROPERTIES
        TIMEOUT 30
        LABELS "benchmark;smoke")
  endif()
endif()
