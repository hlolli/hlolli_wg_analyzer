set(HWA_FUZZERS
  hwa_fuzz_wav
  hwa_fuzz_manifests
  hwa_fuzz_alignment
  hwa_fuzz_probe
  hwa_fuzz_readers
)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(HWA_FUZZ_CORPUS_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/corpus"
)
set(HWA_FUZZ_CORPUS_STAMP
  "${CMAKE_CURRENT_BINARY_DIR}/corpus.stamp"
)
file(GLOB_RECURSE HWA_FUZZ_SEEDS
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/seeds/*"
)

add_custom_command(
  OUTPUT "${HWA_FUZZ_CORPUS_STAMP}"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/make_corpus.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/seeds"
          "${HWA_FUZZ_CORPUS_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${HWA_FUZZ_CORPUS_STAMP}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/make_corpus.py"
          ${HWA_FUZZ_SEEDS}
  VERBATIM
)

add_custom_target(hwa_fuzz_corpus
  DEPENDS "${HWA_FUZZ_CORPUS_STAMP}"
)

if(MSVC)
  target_compile_options(hlolli_wg_analyzer_core PRIVATE
    /fsanitize=address
    /fsanitize=fuzzer
  )
else()
  target_compile_options(hlolli_wg_analyzer_core PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer
  )
endif()

function(hwa_fuzzer target_name source_file corpus_name)
  add_executable(${target_name}
    ${source_file}
    fuzz_support.c
  )
  target_include_directories(${target_name}
    PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  )
  target_link_libraries(${target_name} PRIVATE hlolli_wg_analyzer_core)
  add_dependencies(${target_name} hwa_fuzz_corpus)
  hwa_set_warnings(${target_name})
  if(MSVC)
    target_compile_options(${target_name} PRIVATE
      /fsanitize=address
      /fsanitize=fuzzer
    )
    target_link_options(${target_name} PRIVATE
      /fsanitize=address
      /fsanitize=fuzzer
    )
  else()
    target_compile_options(${target_name} PRIVATE
      -fsanitize=fuzzer,address,undefined
      -fno-omit-frame-pointer
    )
    target_link_options(${target_name} PRIVATE
      -fsanitize=fuzzer,address,undefined
    )
  endif()
  if(BUILD_TESTING)
    add_test(
      NAME fuzz.${corpus_name}-smoke
      COMMAND ${target_name}
              -runs=1
              -max_total_time=15
              -max_len=1048576
              -timeout=10
              -rss_limit_mb=1024
              "${HWA_FUZZ_CORPUS_DIR}/${corpus_name}"
    )
    set_tests_properties(fuzz.${corpus_name}-smoke
      PROPERTIES TIMEOUT 60
    )
  endif()
endfunction()

hwa_fuzzer(hwa_fuzz_wav fuzz_wav.c wav)
hwa_fuzzer(hwa_fuzz_manifests fuzz_manifests.c manifests)
hwa_fuzzer(hwa_fuzz_alignment fuzz_alignment.c alignment)
hwa_fuzzer(hwa_fuzz_probe fuzz_probe.c probe)
hwa_fuzzer(hwa_fuzz_readers fuzz_readers.c readers)
