# Audio DSP Test Project
#
# Offline harness for the DSP chain in Audio/src/DSP.cpp. DSPs are constructed
# directly and driven through a stub AudioBase (TestAudioBase.hpp), because the
# real path (AudioBase::AddDSP) locks a live Audio_Impl.

set(SRCROOT ${CMAKE_CURRENT_SOURCE_DIR}/src/)
set(INCROOT ${CMAKE_CURRENT_SOURCE_DIR}/include/)
set(PCHROOT ${CMAKE_CURRENT_SOURCE_DIR}/)

# Find files used for project
# Recursive so src/reference/*.cpp (ksmaudio transcriptions) and nested test
# files are picked up without editing this file.
file(GLOB_RECURSE SRC "${SRCROOT}*.cpp" "${SRCROOT}*.hpp")
source_group("Sources" FILES ${SRC})

file(GLOB INC "${INCROOT}*.hpp")
source_group("Public Headers" FILES ${INC})

set(TESTS_AUDIO_SRC ${SRC} ${INC})

set(PCH_SRC ${PCHROOT}/stdafx.cpp)
set(PCH_INC ${PCHROOT}/stdafx.h)
set(PCH_FILES ${PCH_SRC} ${PCH_INC})
source_group("" FILES ${PCH_FILES})

# Compiler stuff
enable_precompiled_headers("${TESTS_AUDIO_SRC}" ${PCH_SRC})

add_executable(Tests.Audio ${TESTS_AUDIO_SRC} ${PCH_FILES})
target_compile_features(Tests.Audio PUBLIC cxx_std_17)
set_output_postfixes(Tests.Audio)
target_include_directories(Tests.Audio PRIVATE
    ${SRCROOT}
    ${INCROOT}
    ${PCHROOT}
)
# Dependencies
# No Graphics/GUI: this target renders DSP blocks into memory, never to a window.
target_link_libraries(Tests.Audio Shared)
target_link_libraries(Tests.Audio Audio)
target_link_libraries(Tests.Audio Beatmap)
target_link_libraries(Tests.Audio Tests)

if(MSVC)
    target_compile_options(Tests.Audio PRIVATE /MP)
endif(MSVC)
