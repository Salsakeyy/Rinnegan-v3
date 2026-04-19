#pragma once

#include "thread.h"

class Search {
public:
    Search(TranspositionTable& tt) : shared(tt) {}

    void go(Position& pos, const SearchLimits& limits, bool printOutput = true);
    void stop() { shared.stopped.store(true, std::memory_order_relaxed); }

    static void init();

    int64_t lastNodes() const { return lastNodes_; }

private:
    int negamax(ThreadData& td, int alpha, int beta, int depth, int ply, bool doNull);
    int quiescence(ThreadData& td, int alpha, int beta, int ply);
    bool onNode(ThreadData& td, int ply);
    void workerLoop(ThreadData& td);
    void checkTime();
    void allocateTime(const SearchLimits& limits, Color side);
    void initThreadData(ThreadData& td, const Position& root, bool useNNUE);
    void publishDepth(int depth, int score, Move bestMove, int selDepth);

    SearchShared shared;
    bool printOutput_ = true;
    int64_t lastNodes_ = 0;
};
