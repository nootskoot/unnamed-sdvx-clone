# Offline harnesses for the vendored ksmaudio effects (shimmed Stream, no BASS, no game deps).
set(_fx_tests Tests.AudioEffects)

add_executable(Tests.AudioEffects src/harness.cpp)
set_target_properties(Tests.AudioEffects PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_link_libraries(Tests.AudioEffects PRIVATE AudioEffects)

# KsmEchoDSP is compiled as C++17 (like the Audio target) against a stand-in DSP base, to prove the wrapper
# builds in USC's language standard and to test it without the game. The sources are COPIED into the build
# dir first: a quoted #include "AudioBase.hpp" searches the including file's own directory before -I paths,
# which in the real tree would pick up USC's real AudioBase.hpp instead of the stand-in.
set(_w ${CMAKE_CURRENT_BINARY_DIR}/echo_wrapper)
configure_file(${CMAKE_SOURCE_DIR}/Audio/src/KsmEchoDSP.cpp ${_w}/KsmEchoDSP.cpp COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/Audio/include/Audio/KsmEchoDSP.hpp ${_w}/KsmEchoDSP.hpp COPYONLY)
add_library(KsmEchoDSP_test STATIC ${_w}/KsmEchoDSP.cpp)
set_target_properties(KsmEchoDSP_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(KsmEchoDSP_test PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/usc_stub ${_w})
target_link_libraries(KsmEchoDSP_test PUBLIC AudioEffects)

add_executable(Tests.AudioEffectsEcho src/echo_wrapper_check.cpp)
set_target_properties(Tests.AudioEffectsEcho PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_link_libraries(Tests.AudioEffectsEcho PRIVATE KsmEchoDSP_test)

if(NOT MSVC)
  option(USC_FX_SANITIZE "ASan+UBSan for Tests.AudioEffects*" ON)
  if(USC_FX_SANITIZE)
    foreach(t Tests.AudioEffects Tests.AudioEffectsEcho)
      target_compile_options(${t} PRIVATE -fsanitize=address,undefined -g)
      target_link_options(${t} PRIVATE -fsanitize=address,undefined)
    endforeach()
  endif()
endif()
enable_testing()
add_test(NAME AudioEffects.harness COMMAND Tests.AudioEffects)
add_test(NAME AudioEffects.echo COMMAND Tests.AudioEffectsEcho)

# Demo (not a test): prints ksmaudio Echo vs a transcription of USC's EchoDSP on the same impulse train.
add_executable(Tests.AudioEffectsEchoCompare src/echo_compare.cpp)
set_target_properties(Tests.AudioEffectsEchoCompare PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_link_libraries(Tests.AudioEffectsEchoCompare PRIVATE AudioEffects)
