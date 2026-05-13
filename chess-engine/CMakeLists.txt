cmake_minimum_required(VERSION 3.16)
project(RinneganChess LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(RINNEGAN_LTO "Enable Link-Time Optimization" ON)
option(RINNEGAN_NATIVE "Build for the host CPU instead of portable AVX2 on x86_64" OFF)
set(RINNEGAN_PGO "off" CACHE STRING "PGO mode: off, generate, use")
set_property(CACHE RINNEGAN_PGO PROPERTY STRINGS off generate use)

# Common source files (everything except main.cpp)
set(ENGINE_SOURCES
    src/bitboard.cpp
    src/magics.cpp
    src/zobrist.cpp
    src/position.cpp
    src/movegen.cpp
    src/eval.cpp
    src/policy.cpp
    src/perft.cpp
    src/see.cpp
    src/search.cpp
    src/uci.cpp
)

set(COMMON_WARNING_FLAGS)
set(COMMON_RELEASE_FLAGS)
set(COMMON_RELEASE_LINK_FLAGS)

if(MSVC)
    list(APPEND COMMON_WARNING_FLAGS /W4)
    list(APPEND COMMON_RELEASE_FLAGS /O2)

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x86)$")
        list(APPEND COMMON_RELEASE_FLAGS /arch:AVX2)
    endif()

    if(RINNEGAN_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
        list(APPEND COMMON_RELEASE_FLAGS /GL)
        list(APPEND COMMON_RELEASE_LINK_FLAGS /LTCG)
    endif()
else()
    list(APPEND COMMON_WARNING_FLAGS -Wall -Wextra)
    list(APPEND COMMON_RELEASE_FLAGS -O3)

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x86)$")
        if(RINNEGAN_NATIVE)
            list(APPEND COMMON_RELEASE_FLAGS -march=native)
        else()
            list(APPEND COMMON_RELEASE_FLAGS -mavx2 -mbmi -mbmi2 -mpopcnt)
        endif()
    endif()

    if(RINNEGAN_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
        list(APPEND COMMON_RELEASE_FLAGS -flto)
        list(APPEND COMMON_RELEASE_LINK_FLAGS -flto)
    endif()

    if(MINGW)
        list(APPEND COMMON_RELEASE_LINK_FLAGS -static -static-libgcc -static-libstdc++)
    endif()

    set(RINNEGAN_PGO_DIR "${CMAKE_BINARY_DIR}/pgo")
    if(RINNEGAN_PGO STREQUAL "generate")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            list(APPEND COMMON_RELEASE_FLAGS -fprofile-generate=${RINNEGAN_PGO_DIR})
            list(APPEND COMMON_RELEASE_LINK_FLAGS -fprofile-generate=${RINNEGAN_PGO_DIR})
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            list(APPEND COMMON_RELEASE_FLAGS -fprofile-generate=${RINNEGAN_PGO_DIR})
            list(APPEND COMMON_RELEASE_LINK_FLAGS -fprofile-generate=${RINNEGAN_PGO_DIR})
        endif()
    elseif(RINNEGAN_PGO STREQUAL "use")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # Clang expects a merged .profdata file; orchestration script runs llvm-profdata merge first.
            list(APPEND COMMON_RELEASE_FLAGS -fprofile-use=${RINNEGAN_PGO_DIR}/merged.profdata
                                             -Wno-profile-instr-unprofiled
                                             -Wno-profile-instr-out-of-date)
            list(APPEND COMMON_RELEASE_LINK_FLAGS -fprofile-use=${RINNEGAN_PGO_DIR}/merged.profdata)
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            list(APPEND COMMON_RELEASE_FLAGS -fprofile-use=${RINNEGAN_PGO_DIR} -fprofile-correction)
            list(APPEND COMMON_RELEASE_LINK_FLAGS -fprofile-use=${RINNEGAN_PGO_DIR})
        endif()
    endif()
endif()

# Engine executable
add_executable(engine ${ENGINE_SOURCES} src/main.cpp)
target_compile_options(engine PRIVATE ${COMMON_WARNING_FLAGS})
target_compile_options(engine PRIVATE $<$<CONFIG:Release>:${COMMON_RELEASE_FLAGS}>)
target_link_options(engine PRIVATE $<$<CONFIG:Release>:${COMMON_RELEASE_LINK_FLAGS}>)
target_compile_definitions(engine PRIVATE $<$<CONFIG:Release>:NDEBUG>)

# Perft test executable
add_executable(perft ${ENGINE_SOURCES} tests/perft_main.cpp)
target_compile_options(perft PRIVATE ${COMMON_WARNING_FLAGS})
target_compile_options(perft PRIVATE $<$<CONFIG:Release>:${COMMON_RELEASE_FLAGS}>)
target_link_options(perft PRIVATE $<$<CONFIG:Release>:${COMMON_RELEASE_LINK_FLAGS}>)
target_compile_definitions(perft PRIVATE $<$<CONFIG:Release>:NDEBUG>)

# Fast policy dataset packer
add_executable(pack_policy_dataset_cpp
    src/bitboard.cpp
    src/magics.cpp
    src/zobrist.cpp
    src/position.cpp
    src/movegen.cpp
    src/see.cpp
    tools/policy/pack_policy_dataset_cpp.cpp
)
target_include_directories(pack_policy_dataset_cpp PRIVATE src)
target_compile_options(pack_policy_dataset_cpp PRIVATE ${COMMON_WARNING_FLAGS})
target_compile_options(pack_policy_dataset_cpp PRIVATE $<$<CONFIG:Release>:${COMMON_RELEASE_FLAGS}>)
target_link_options(pack_policy_dataset_cpp PRIVATE $<$<CONFIG:Release>:${COMMON_RELEASE_LINK_FLAGS}>)
target_compile_definitions(pack_policy_dataset_cpp PRIVATE $<$<CONFIG:Release>:NDEBUG>)

# Threading
find_package(Threads REQUIRED)
target_link_libraries(engine Threads::Threads)
target_link_libraries(perft Threads::Threads)
