#pragma once

#include "movegen.h"
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
    Move searchMoves[MAX_MOVES] = {};
    int searchMoveCount = 0;
};

struct ThreadData {
    static constexpr int STATE_STACK_SIZE = MAX_PLY + 64;

    Position pos;
    StateInfo rootState;
    StateInfo stateStack[STATE_STACK_SIZE] = {};
    int staticEvalStack[MAX_PLY] = {};
    Move killers[MAX_PLY][2] = {};
    int history[2][64][64] = {};
    Move counter[2][64][64] = {};
    Move prevMoveStack[MAX_PLY] = {};
    int64_t nodes = 0;
    int selDepth = 0;
    int threadId = 0;
    Move bestMove = MOVE_NONE;
    int bestScore = 0;
    int completedDepth = 0;
    int stableIters = 0;
    Move prevBest = MOVE_NONE;
};

struct SearchShared {
    explicit SearchShared(TranspositionTable& ttRef) : tt(ttRef) {}

    TranspositionTable& tt;
    std::atomic<bool> stopped{false};
    std::atomic<int64_t> globalNodes{0};
    std::atomic<int> completedDepth{0};
    SearchLimits limits;
    std::chrono::time_point<std::chrono::steady_clock> startTime;
    int softLimit = 0;
    int hardLimit = 0;

    // Root policy cache: computed once per `go` and reused across all
    // iterative-deepening iterations. Order matches generateLegal() output
    // for the root position, which is deterministic.
    Move rootPolicyMoves[MAX_MOVES] = {};
    int rootPolicyBonus[MAX_MOVES] = {};
    int rootPolicyCount = 0;
    bool rootPolicyValid = false;
    // top1 - top2 margin over rootPolicyBonus. 0 when not valid or <2 moves.
    int policyConfidence = 0;
};
