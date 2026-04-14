#pragma once

#include "position.h"
#include "tt.h"
#include <atomic>
#include <chrono>

struct SearchLimits {
    int depth = MAX_PLY;
    int64_t nodes = 0; // 0 = unlimited
    int movetime = 0;  // ms, 0 = use wtime/btime
    int wtime = 0;
    int btime = 0;
    int winc = 0;
    int binc = 0;
    int movestogo = 0;
    bool infinite = false;
};

struct SearchInfo {
    int64_t nodes = 0;
    Move bestMove = MOVE_NONE;
    int score = 0;
    int depth = 0;
    std::chrono::time_point<std::chrono::steady_clock> startTime;
    int allocatedTime = 0; // ms
    Move killers[MAX_PLY][2] = {};
    int history[2][64][64] = {};
};

class Search {
public:
    Search(TranspositionTable& tt) : tt(tt) {}

    void go(Position& pos, const SearchLimits& limits);
    void stop() { stopped.store(true); }

    std::atomic<bool> stopped{false};

private:
    int negamax(Position& pos, int alpha, int beta, int depth, int ply, bool doNull);
    int quiescence(Position& pos, int alpha, int beta, int ply);
    void checkTime();
    int allocateTime(const SearchLimits& limits, Color side);

    TranspositionTable& tt;
    SearchInfo info;
};
