# Beatmap library

set(ENV{ASAN_OPTIONS}, "new_delete_type_mismatch=0")

set(INCROOT ${CMAKE_CURRENT_SOURCE_DIR}/include/Beatmap/)
set(SRCROOT ${CMAKE_CURRENT_SOURCE_DIR}/src/)
set(PCHROOT ${CMAKE_CURRENT_SOURCE_DIR}/)

file(GLOB INC "${INCROOT}/*.hpp")
source_group("Public Headers" FILES ${INC})

file(GLOB SRC "${SRCROOT}/*.cpp" "${SRCROOT}/*.hpp")
source_group("Sources" FILES ${SRC})

# Find files used for project
set(BEATMAP_SRC ${SRC} ${INC})

set(PCH_SRC ${PCHROOT}/stdafx.cpp)
set(PCH_INC ${PCHROOT}/stdafx.h)
set(PCH_FILES ${PCH_SRC} ${PCH_INC})
source_group("" FILES ${PCH_FILES})

# Compiler stuff
enable_precompiled_headers("${BEATMAP_SRC}" ${PCH_SRC})

add_library(Beatmap ${BEATMAP_SRC} ${PCH_FILES})
target_compile_features(Beatmap PUBLIC cxx_std_17)
target_include_directories(Beatmap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(Beatmap PRIVATE
    ${INCROOT}
    ${SRCROOT}
    ${PCHROOT}
)

# Dependencies

## Local Dependencies
target_link_libraries(Beatmap Shared)
target_link_libraries(Beatmap BackbeatIntegration)

## 3rd Party dependencies
target_link_libraries(Beatmap sqlite3)
target_link_libraries(Beatmap nlohmann_json)

target_link_libraries(Beatmap cc-common)

# Enable multiprocess compiling
if(MSVC)
    target_compile_options(Beatmap PRIVATE /MP)
endif(MSVC)

# KSH v2 importer: isolate upstream's C++20 requirement from USC's C++17 code.
set(KSH2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/kson_compat")
file(GLOB_RECURSE KSH2_SRC CONFIGURE_DEPENDS "${KSH2_ROOT}/src/*.cpp")
add_library(USCKSH2 STATIC KSH2Bridge.cpp ${KSH2_SRC})
target_compile_features(USCKSH2 PRIVATE cxx_std_20)
target_include_directories(USCKSH2 PRIVATE "${KSH2_ROOT}/include" "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(USCKSH2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(USCKSH2 PRIVATE /utf-8)
endif()
if(NOT WIN32 AND NOT EMSCRIPTEN)
    find_package(Iconv REQUIRED)
    target_link_libraries(USCKSH2 PRIVATE Iconv::Iconv)
endif()
target_link_libraries(Beatmap USCKSH2)
