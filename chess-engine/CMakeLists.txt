cmake_minimum_required(VERSION 3.16)
project(RinneganChess LANGUAGES CXX)

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
    src/perft.cpp
    src/search.cpp
    src/uci.cpp
)

# Engine executable
add_executable(engine ${ENGINE_SOURCES} src/main.cpp)
target_compile_options(engine PRIVATE -Wall -Wextra -O3 -DNDEBUG)

# Perft test executable
add_executable(perft ${ENGINE_SOURCES} tests/perft_main.cpp)
target_compile_options(perft PRIVATE -Wall -Wextra -O3 -DNDEBUG)

# Threading
find_package(Threads REQUIRED)
target_link_libraries(engine Threads::Threads)
target_link_libraries(perft Threads::Threads)
