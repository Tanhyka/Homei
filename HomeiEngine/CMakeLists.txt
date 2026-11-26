cmake_minimum_required(VERSION 3.20)
project(HomeiEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HOMEI_STATS "Enable search profiling statistics" OFF)
if(HOMEI_STATS)
    add_compile_definitions(HOMEI_STATS)
endif()

option(HOMEI_PROFILE "Enable runtime SearchProfile timing instrumentation" OFF)
if(HOMEI_PROFILE)
    add_compile_definitions(HOMEI_PROFILE)
endif()

set(HOMEI_PGO_MODE "OFF" CACHE STRING "MSVC PGO mode: OFF, GEN (instrument), USE (optimize)")
set(HOMEI_PGO_DB "${CMAKE_BINARY_DIR}/homei_pgo.pgd" CACHE FILEPATH "Path to MSVC PGO database (.pgd)")

set(HOMEI_SOURCES
    src/geometry.cpp
    src/bitboard.cpp
    src/zobrist.cpp
    src/base64url.cpp
    src/json.cpp
    src/lppn.cpp
    src/state_json.cpp
    src/attack.cpp
    src/movegen.cpp
    src/notation.cpp
    src/adjudicate.cpp
    src/book.cpp
    src/see.cpp
    src/eval.cpp
    src/nnue/nnue.cpp
    src/tt.cpp
    src/mate.cpp
    src/search.cpp
)

add_library(homei_core STATIC ${HOMEI_SOURCES})
target_include_directories(homei_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
    target_compile_options(homei_core PRIVATE /W3 /O2 /Ob2 /Oi /Ot /GL /arch:AVX2 /utf-8)
else()
    target_compile_options(homei_core PRIVATE -Wall -Wextra -O3 -march=x86-64-v3 -flto)
endif()

find_package(Threads REQUIRED)
target_link_libraries(homei_core PUBLIC Threads::Threads)

add_executable(clp_core_cli src/clp_ep.cpp)
target_link_libraries(clp_core_cli PRIVATE homei_core)

add_executable(homei src/clp_ep.cpp)
target_link_libraries(homei PRIVATE homei_core)

add_executable(texel_extract tools_cpp/texel_extract.cpp)
target_link_libraries(texel_extract PRIVATE homei_core)

add_executable(selfplay_gen tools_cpp/selfplay_gen.cpp)
target_link_libraries(selfplay_gen PRIVATE homei_core)

add_library(clp_fast_core SHARED src/fast_core_dll.cpp)
target_link_libraries(clp_fast_core PRIVATE homei_core)
set_target_properties(clp_fast_core PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    PREFIX ""
)

add_library(nnue_dataloader SHARED tools_cpp/nnue_dataloader_dll.cpp)
target_link_libraries(nnue_dataloader PRIVATE homei_core)
set_target_properties(nnue_dataloader PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    PREFIX ""
)
if(MSVC)
    target_link_options(nnue_dataloader PRIVATE /LTCG)
endif()

foreach(tgt IN ITEMS clp_core_cli homei texel_extract selfplay_gen)
    if(MSVC)
        if(tgt STREQUAL "homei" AND HOMEI_PGO_MODE STREQUAL "GEN")
            target_link_options(${tgt} PRIVATE /STACK:16777216 /LTCG:PGINSTRUMENT /PGD:${HOMEI_PGO_DB})
        elseif(tgt STREQUAL "homei" AND HOMEI_PGO_MODE STREQUAL "USE")
            target_link_options(${tgt} PRIVATE /STACK:16777216 /LTCG:PGOPTIMIZE /PGD:${HOMEI_PGO_DB})
        else()
            target_link_options(${tgt} PRIVATE /STACK:16777216 /LTCG)
        endif()
    else()
        target_link_options(${tgt} PRIVATE -flto)
    endif()
    set_target_properties(${tgt} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    )
endforeach()
