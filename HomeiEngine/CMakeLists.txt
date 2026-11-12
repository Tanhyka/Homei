cmake_minimum_required(VERSION 3.20)
project(HomeiEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HOMEI_STATS "Enable search profiling statistics" OFF)
if(HOMEI_STATS)
    add_compile_definitions(HOMEI_STATS)
endif()

set(HOMEI_SOURCES
    src/geometry.cpp
    src/zobrist.cpp
    src/base64url.cpp
    src/json.cpp
    src/lppn.cpp
    src/state_json.cpp
    src/attack.cpp
    src/movegen.cpp
    src/notation.cpp
    src/adjudicate.cpp
    src/see.cpp
    src/eval.cpp
    src/tt.cpp
    src/mate.cpp
    src/search.cpp
)

add_library(homei_core STATIC ${HOMEI_SOURCES})
target_include_directories(homei_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
    target_compile_options(homei_core PRIVATE /W3 /O2 /Ob2 /Oi /Ot /utf-8)
else()
    target_compile_options(homei_core PRIVATE -Wall -Wextra -O3 -march=x86-64-v3)
endif()

find_package(Threads REQUIRED)
target_link_libraries(homei_core PUBLIC Threads::Threads)

add_executable(clp_core_cli src/clp_ep.cpp)
target_link_libraries(clp_core_cli PRIVATE homei_core)

add_executable(homei src/clp_ep.cpp)
target_link_libraries(homei PRIVATE homei_core)

foreach(tgt IN ITEMS clp_core_cli homei)
    if(MSVC)
        target_link_options(${tgt} PRIVATE /STACK:16777216)
    endif()
    set_target_properties(${tgt} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    )
endforeach()
