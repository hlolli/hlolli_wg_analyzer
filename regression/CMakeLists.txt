set(HWA_WAV_REGRESSION_SOURCES
    wav_regression.c
    ${PROJECT_SOURCE_DIR}/src/wav_reader.c)

add_executable(hwa_wav_regression
    regression_runner.c
    ${HWA_WAV_REGRESSION_SOURCES})
target_include_directories(hwa_wav_regression PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src)
if(NOT MSVC)
  target_link_libraries(hwa_wav_regression PRIVATE m)
endif()
hwa_set_warnings(hwa_wav_regression)

if(BUILD_TESTING)
  add_test(NAME regression.wav-malformed
      COMMAND hwa_wav_regression
              ${CMAKE_CURRENT_SOURCE_DIR}/corpus/wav/pcm16-mono.hex
              ${CMAKE_CURRENT_SOURCE_DIR}/corpus/wav/pcm8-odd-chunk.hex
              ${CMAKE_CURRENT_SOURCE_DIR}/corpus/wav/rf64-pcm16.hex
              ${CMAKE_CURRENT_SOURCE_DIR}/corpus/wav/float32-nan.hex
              ${CMAKE_CURRENT_SOURCE_DIR}/corpus/wav/truncated.hex)
  set_tests_properties(regression.wav-malformed PROPERTIES
      TIMEOUT 30
      LABELS "wav;regression")
endif()
