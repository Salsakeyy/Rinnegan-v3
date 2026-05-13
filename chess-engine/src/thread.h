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
    // (V7) Piece moved at each ply — mirrors prevMoveStack but stores the
    // piece-type-and-color, since the board has already moved when we look
    // it up in the child node. Used as contHist's "previous" key.
    Piece movedPieceStack[MAX_PLY] = {};
    // (V7) 1-ply continuation history. [prevPiece][prevTo][curPiece][curTo].
    // Index 12 (NO_PIECE) reserved for "no previous move".
    int16_t contHist[13][64][13][64] = {};
    // (V7) Capture history keyed by (movedPiece, toSq, capturedPT). Used to
    // disambiguate captures with identical MVV-LVA.
    int16_t captHist[13][64][7] = {};
    // (V7) Excluded move per ply for singular-extension search. MOVE_NONE
    // when not in a singular sub-search.
    Move excludedMoveStack[MAX_PLY] = {};
    // (V7) Per-root-move node accounting. Index matches the root move list
    // order. Used by the dynamic time manager to weight by node share of
    // the best root move.
    int64_t rootMoveNodes[MAX_MOVES] = {};
    int64_t nodes = 0;
    int selDepth = 0;
    int threadId = 0;
    Move bestMove = MOVE_NONE;
    int bestScore = 0;
    int completedDepth = 0;
    int stableIters = 0;
    Move prevBest = MOVE_NONE;
    // (V7) Best-move-changes tracker (decays each iteration).
    int bestMoveChanges = 0;
    // (V7) Index of bestMove in the current iteration's root move list,
    // used to weight time by best-move node share.
    int bestRootIdx = 0;
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
