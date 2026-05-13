#pragma once

#include "thread.h"

class Search {
public:
    Search(TranspositionTable& tt) : shared(tt) {}

    void go(Position& pos, const SearchLimits& limits, bool printOutput = true);
    void stop() { shared.stopped.store(true, std::memory_order_relaxed); }

    static void init();

    int64_t lastNodes() const { return lastNodes_; }
    Move lastBestMove() const { return lastBestMove_; }

private:
    int negamax(ThreadData& td, int alpha, int beta, int depth, int ply, bool doNull, bool cutNode);
    int quiescence(ThreadData& td, int alpha, int beta, int ply);
    bool onNode(ThreadData& td, int ply);
    void workerLoop(ThreadData& td);
    void checkTime();
    void allocateTime(const SearchLimits& limits, Color side);
    void initThreadData(ThreadData& td, const Position& root);
    void publishDepth(int depth, int score, Move bestMove, int selDepth);

    SearchShared shared;
    bool printOutput_ = true;
    int64_t lastNodes_ = 0;
    Move lastBestMove_ = MOVE_NONE;
};
