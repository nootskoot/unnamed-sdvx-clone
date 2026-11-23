# Vendored kshootmania/ksmaudio effects (commit 5c2466d), BASS-free. See THIRD_PARTY_LICENSE.txt.
#   include/ksmaudio, src/AudioEffect : verbatim vendored code (do not edit; changes live in patches/ or here)
#   shim/                             : offline stand-in for ksmaudio/Stream.hpp + Backend.hpp
#   usc/                              : USC-authored C++17-safe façade (usc/KsmEchoCore.hpp)
#
# The vendored code needs C++20. That requirement is PRIVATE: linking AudioEffects does not raise the
# language standard of the target that links it. Include only usc/*.hpp from C++17 code; include the
# ksmaudio headers directly only from C++20 translation units (as Tests.AudioEffects does).
option(USC_FX_USE_SHIM "Use the offline Stream shim for ksmaudio/Stream.hpp + Backend.hpp" ON)

file(GLOB_RECURSE USC_FX_SRC CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/usc/src/*.cpp")
add_library(AudioEffects STATIC ${USC_FX_SRC})
set_target_properties(AudioEffects PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
                      POSITION_INDEPENDENT_CODE ON)
target_include_directories(AudioEffects PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/usc/include)
if(USC_FX_USE_SHIM)
  target_include_directories(AudioEffects BEFORE PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/shim)
endif()
