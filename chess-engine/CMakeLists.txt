cmake_minimum_required(VERSION 3.16)
project(RinneganChess LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Common source files (everything except main.cpp)
set(ENGINE_SOURCES
    src/bitboard.cpp
    src/magics.cpp
    src/zobrist.cpp
    src/position.cpp
    src/movegen.cpp
    src/eval.cpp
    src/nnue.cpp
    src/perft.cpp
    src/search.cpp
    src/uci.cpp
)

set(COMMON_WARNING_FLAGS -Wall -Wextra)
set(COMMON_RELEASE_FLAGS -O3)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x86)$")
    list(APPEND COMMON_RELEASE_FLAGS -march=native -mavx2 -mbmi -mbmi2 -mpopcnt)
endif()

# Engine executable
add_executable(engine ${ENGINE_SOURCES} src/main.cpp)
target_compile_options(engine PRIVATE ${COMMON_WARNING_FLAGS})
target_compile_options(engine PRIVATE $<$<CONFIG:Release>:${COMMON_RELEASE_FLAGS}>)
target_compile_definitions(engine PRIVATE $<$<CONFIG:Release>:NDEBUG>)

# Perft test executable
add_executable(perft ${ENGINE_SOURCES} tests/perft_main.cpp)
target_compile_options(perft PRIVATE ${COMMON_WARNING_FLAGS})
target_compile_options(perft PRIVATE $<$<CONFIG:Release>:${COMMON_RELEASE_FLAGS}>)
target_compile_definitions(perft PRIVATE $<$<CONFIG:Release>:NDEBUG>)

# Threading
find_package(Threads REQUIRED)
target_link_libraries(engine Threads::Threads)
target_link_libraries(perft Threads::Threads)
