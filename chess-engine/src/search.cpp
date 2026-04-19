#include "search.h"
#include "eval.h"
#include "movegen.h"
#include "uci.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

// ---------- Move ordering ----------

static const int MVV_LVA[7][7] = {
    { 105, 205, 305, 405, 505, 605, 0 },
    { 104, 204, 304, 404, 504, 604, 0 },
    { 103, 203, 303, 403, 503, 603, 0 },
    { 102, 202, 302, 402, 502, 602, 0 },
    { 101, 201, 301, 401, 501, 601, 0 },
    { 100, 200, 300, 400, 500, 600, 0 },
    {   0,   0,   0,   0,   0,   0, 0 },
};

static constexpr int HISTORY_MAX = 16384;

static inline int historyBonus(int depth) {
    int bonus = depth * depth;
    if (bonus > 400) bonus = 400;
    return bonus;
}

static inline void updateHistory(int& entry, int bonus) {
    entry += bonus - entry * std::abs(bonus) / HISTORY_MAX;
}

static int scoreMove(const Position& pos, Move move, Move ttMove,
                     const Move killers[2], Move counterMove,
                     const int history[64][64]) {
    if (move == ttMove) return 1000000;

    Square to = move.to();
    Piece captured = pos.pieceOn(to);
    if (move.flag() == FLAG_ENPASSANT)
        captured = makePiece(~pos.sideToMove(), PAWN);

    if (captured != NO_PIECE) {
        Piece mover = pos.pieceOn(move.from());
        return 100000 + MVV_LVA[pieceType(mover)][pieceType(captured)];
    }

    if (move.flag() == FLAG_PROMOTION) return 90000 + move.promoPiece();
    if (move == killers[0]) return 80000;
    if (move == killers[1]) return 79000;
    if (counterMove && move == counterMove) return 78000;

    return history[move.from()][move.to()];
}

static void sortMoves(const Position& pos, MoveList& moves, int* scores,
                      Move ttMove, const Move killers[2], Move counterMove,
                      const int history[64][64]) {
    for (int i = 0; i < moves.count; ++i)
        scores[i] = scoreMove(pos, moves[i], ttMove, killers, counterMove, history);

    for (int i = 1; i < moves.count; ++i) {
        Move move = moves[i];
        int score = scores[i];
        int j = i - 1;
        while (j >= 0 && scores[j] < score) {
            moves[j + 1] = moves[j];
            scores[j + 1] = scores[j];
            --j;
        }
        moves[j + 1] = move;
        scores[j + 1] = score;
    }
}

static int scoreCaptureMove(const Position& pos, Move move) {
    Piece captured = pos.pieceOn(move.to());
    if (move.flag() == FLAG_ENPASSANT)
        captured = makePiece(~pos.sideToMove(), PAWN);
    if (captured == NO_PIECE) return 0;

    Piece mover = pos.pieceOn(move.from());
    return MVV_LVA[pieceType(mover)][pieceType(captured)];
}

static void sortCaptures(const Position& pos, MoveList& moves) {
    int scores[MAX_MOVES];
    for (int i = 0; i < moves.count; ++i)
        scores[i] = scoreCaptureMove(pos, moves[i]);

    for (int i = 1; i < moves.count; ++i) {
        Move move = moves[i];
        int score = scores[i];
        int j = i - 1;
        while (j >= 0 && scores[j] < score) {
            moves[j + 1] = moves[j];
            scores[j + 1] = scores[j];
            --j;
        }
        moves[j + 1] = move;
        scores[j + 1] = score;
    }
}

// ---------- TT score <-> search score mate adjustment ----------

static int scoreToTT(int score, int ply) {
    if (score >= SCORE_MATE_IN_MAX_PLY)  return score + ply;
    if (score <= -SCORE_MATE_IN_MAX_PLY) return score - ply;
    return score;
}

static int scoreFromTT(int score, int ply) {
    if (score >= SCORE_MATE_IN_MAX_PLY)  return score - ply;
    if (score <= -SCORE_MATE_IN_MAX_PLY) return score + ply;
    return score;
}

// ---------- LMR table ----------

static int lmrTable[MAX_PLY + 1][MAX_MOVES];

namespace {

inline int elapsedMs(const SearchShared& shared) {
    auto now = std::chrono::steady_clock::now();
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(now - shared.startTime).count());
}

inline void storeMax(std::atomic<int>& target, int value) {
    int current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

inline const NNUE::Accumulator* currentAccumulator(const ThreadData& td) {
    return td.useNNUE ? &td.accStack[td.accIdx] : nullptr;
}

inline void pushAccumulator(ThreadData& td) {
    assert(td.accIdx + 1 < ThreadData::ACC_STACK_SIZE);
    td.accStack[td.accIdx + 1] = td.accStack[td.accIdx];
    ++td.accIdx;
}

inline void popAccumulator(ThreadData& td) {
    assert(td.accIdx > 0);
    --td.accIdx;
}

inline void applyMoveToAccumulator(NNUE::Accumulator& acc, Color us, Move move,
                                   Piece movedPiece, Piece capturedPiece) {
    Square from = move.from();
    Square to = move.to();

    switch (move.flag()) {
    case FLAG_CASTLING: {
        NNUE::movePiece(acc, us, KING, from, to);
        if (to > from)
            NNUE::movePiece(acc, us, ROOK, Square(from + 3), Square(from + 1));
        else
            NNUE::movePiece(acc, us, ROOK, Square(from - 4), Square(from - 1));
        break;
    }
    case FLAG_ENPASSANT: {
        NNUE::movePiece(acc, us, PAWN, from, to);
        Square capturedSq = (us == WHITE) ? Square(to - 8) : Square(to + 8);
        NNUE::subPiece(acc, ~us, PAWN, capturedSq);
        break;
    }
    case FLAG_PROMOTION: {
        NNUE::subPiece(acc, us, PAWN, from);
        if (capturedPiece != NO_PIECE)
            NNUE::subPiece(acc, ~us, pieceType(capturedPiece), to);
        NNUE::addPiece(acc, us, move.promoPiece(), to);
        break;
    }
    default:
        NNUE::movePiece(acc, us, pieceType(movedPiece), from, to);
        if (capturedPiece != NO_PIECE)
            NNUE::subPiece(acc, ~us, pieceType(capturedPiece), to);
        break;
    }
}

} // namespace

void Search::init() {
    for (int depth = 0; depth <= MAX_PLY; ++depth) {
        for (int moveNumber = 0; moveNumber < MAX_MOVES; ++moveNumber) {
            if (depth == 0 || moveNumber == 0) {
                lmrTable[depth][moveNumber] = 0;
            } else {
                double reduction = 0.75 + std::log(double(depth)) * std::log(double(moveNumber)) / 2.25;
                lmrTable[depth][moveNumber] = std::max(0, int(reduction));
            }
        }
    }
}

bool Search::onNode(ThreadData& td, int ply) {
    if (shared.stopped.load(std::memory_order_relaxed))
        return true;

    td.selDepth = std::max(td.selDepth, ply);
    ++td.nodes;

    int64_t global = shared.globalNodes.fetch_add(1, std::memory_order_relaxed) + 1;
    if (shared.limits.nodes > 0 && global >= shared.limits.nodes)
        shared.stopped.store(true, std::memory_order_relaxed);

    if (td.threadId == 0 && (global & 2047) == 0)
        checkTime();

    return shared.stopped.load(std::memory_order_relaxed);
}

void Search::checkTime() {
    if (shared.stopped.load(std::memory_order_relaxed))
        return;

    if (shared.limits.nodes > 0 &&
        shared.globalNodes.load(std::memory_order_relaxed) >= shared.limits.nodes) {
        shared.stopped.store(true, std::memory_order_relaxed);
        return;
    }

    if (shared.hardLimit <= 0)
        return;

    if (elapsedMs(shared) >= shared.hardLimit)
        shared.stopped.store(true, std::memory_order_relaxed);
}

void Search::allocateTime(const SearchLimits& limits, Color side) {
    shared.softLimit = 0;
    shared.hardLimit = 0;

    if (limits.movetime > 0) {
        shared.softLimit = limits.movetime;
        shared.hardLimit = limits.movetime;
        return;
    }
    if (limits.infinite)
        return;

    int timeLeft = (side == WHITE) ? limits.wtime : limits.btime;
    int inc = (side == WHITE) ? limits.winc : limits.binc;
    if (timeLeft <= 0)
        return;

    int movesToGo = limits.movestogo > 0 ? limits.movestogo : 30;
    int soft = timeLeft / movesToGo + inc * 3 / 4;
    soft = std::max(10, soft);
    soft = std::min(soft, timeLeft / 2);

    int hard = soft * 4;
    int cap = std::max(10, timeLeft / 3);
    hard = std::min(hard, cap);
    hard = std::max(hard, soft);

    shared.softLimit = soft;
    shared.hardLimit = hard;
}

void Search::initThreadData(ThreadData& td, const Position& root, bool useNNUE) {
    td = ThreadData{};
    td.pos.copyFrom(root, td.rootState);
    td.useNNUE = useNNUE;
    td.accIdx = 0;

    if (td.useNNUE)
        NNUE::refresh(td.pos, td.accStack[0]);
}

// ---------- Quiescence ----------

int Search::quiescence(ThreadData& td, int alpha, int beta, int ply) {
    Position& pos = td.pos;

    if (shared.stopped.load(std::memory_order_relaxed))
        return 0;
    if (ply >= ThreadData::STATE_STACK_SIZE - 1)
        return Eval::evaluate(pos, currentAccumulator(td));
    if (onNode(td, ply))
        return 0;

    int standPat = Eval::evaluate(pos, currentAccumulator(td));
    if (standPat >= beta) return beta;
    if (alpha < standPat) alpha = standPat;

    const int DELTA = 1000;
    if (standPat + DELTA < alpha) return alpha;

    MoveList captures;
    MoveGen::generateCaptures(pos, captures);
    sortCaptures(pos, captures);

    Color us = pos.sideToMove();

    for (int i = 0; i < captures.count; ++i) {
        Move move = captures[i];
        Piece movedPiece = pos.pieceOn(move.from());
        Piece capturedPiece = (move.flag() == FLAG_ENPASSANT) ? makePiece(~us, PAWN) : pos.pieceOn(move.to());

        if (td.useNNUE) {
            if (td.accIdx + 1 >= ThreadData::ACC_STACK_SIZE)
                break;
            pushAccumulator(td);
            applyMoveToAccumulator(td.accStack[td.accIdx], us, move, movedPiece, capturedPiece);
        }

        pos.makeMove(move, td.stateStack[ply]);
        if (pos.isSquareAttacked(pos.kingSq(us), pos.sideToMove())) {
            pos.unmakeMove(move);
            if (td.useNNUE) popAccumulator(td);
            continue;
        }

        int score = -quiescence(td, -beta, -alpha, ply + 1);

        pos.unmakeMove(move);
        if (td.useNNUE) popAccumulator(td);

        if (shared.stopped.load(std::memory_order_relaxed))
            return 0;
        if (score >= beta)
            return beta;
        if (score > alpha)
            alpha = score;
    }

    return alpha;
}

// ---------- Main negamax ----------

int Search::negamax(ThreadData& td, int alpha, int beta, int depth, int ply, bool doNull) {
    Position& pos = td.pos;

    if (shared.stopped.load(std::memory_order_relaxed))
        return 0;
    if (ply >= MAX_PLY - 1)
        return Eval::evaluate(pos, currentAccumulator(td));
    if (ply >= ThreadData::STATE_STACK_SIZE - 1)
        return Eval::evaluate(pos, currentAccumulator(td));

    if (ply > 0 && pos.isDraw(ply))
        return SCORE_DRAW;

    if (ply > 0) {
        int mated = -SCORE_MATE + ply;
        int mating = SCORE_MATE - ply - 1;
        if (alpha < mated) alpha = mated;
        if (beta > mating) beta = mating;
        if (alpha >= beta)
            return alpha;
    }

    bool inCheck = pos.inCheck();
    if (inCheck) depth++;
    if (depth <= 0)
        return quiescence(td, alpha, beta, ply);
    if (onNode(td, ply))
        return 0;

    bool pvNode = (beta - alpha > 1);

    bool ttHit = false;
    TTEntry* ttEntry = shared.tt.probe(pos.key(), ttHit);
    Move ttMove = MOVE_NONE;
    int ttStaticEval = SCORE_NONE;
    if (ttHit) {
        ttMove = ttEntry->bestMove;
        if (!ttMoveIsUsable(pos, ttMove)) {
            ttHit = false;
            ttMove = MOVE_NONE;
        } else {
            ttStaticEval = ttEntry->staticEval;
            if (!pvNode && ttEntry->depth >= depth) {
                int ttScore = scoreFromTT(ttEntry->score, ply);
                if (ttEntry->flag == TT_EXACT) return ttScore;
                if (ttEntry->flag == TT_LOWER && ttScore >= beta) return ttScore;
                if (ttEntry->flag == TT_UPPER && ttScore <= alpha) return ttScore;
            }
        }
    }

    int staticEval;
    if (inCheck) {
        staticEval = SCORE_NONE;
    } else if (ttHit && ttStaticEval != SCORE_NONE) {
        staticEval = ttStaticEval;
    } else {
        staticEval = Eval::evaluate(pos, currentAccumulator(td));
    }

    bool prunable = !pvNode && !inCheck && staticEval != SCORE_NONE;

    if (prunable && depth <= 6) {
        int margin = 80 * depth;
        if (staticEval - margin >= beta)
            return staticEval;
    }

    if (prunable && depth <= 2) {
        if (staticEval + 300 < alpha) {
            int razorScore = quiescence(td, alpha - 1, alpha, ply);
            if (razorScore < alpha)
                return razorScore;
        }
    }

    if (doNull && !inCheck && !pvNode && depth >= 3 &&
        staticEval != SCORE_NONE && staticEval >= beta) {
        Bitboard nonPawnMaterial = pos.pieces(pos.sideToMove()) &
            ~pos.pieces(pos.sideToMove(), PAWN) &
            ~pos.pieces(pos.sideToMove(), KING);
        if (nonPawnMaterial) {
            int reduction = 2 + depth / 4;
            if (td.useNNUE) {
                if (td.accIdx + 1 >= ThreadData::ACC_STACK_SIZE)
                    return staticEval;
                pushAccumulator(td);
            }

            pos.doNullMove(td.stateStack[ply]);
            int nullScore = -negamax(td, -beta, -beta + 1, depth - reduction - 1, ply + 1, false);
            pos.undoNullMove();
            if (td.useNNUE) popAccumulator(td);

            if (shared.stopped.load(std::memory_order_relaxed))
                return 0;
            if (nullScore >= beta)
                return beta;
        }
    }

    MoveList moves;
    MoveGen::generateLegal(pos, moves);
    if (moves.count == 0)
        return inCheck ? (-SCORE_MATE + ply) : SCORE_DRAW;

    Move prevMove = (ply > 0) ? td.prevMoveStack[ply - 1] : MOVE_NONE;
    Color us = pos.sideToMove();
    Move counterMove = MOVE_NONE;
    if (prevMove)
        counterMove = td.counter[us][prevMove.from()][prevMove.to()];

    int scores[MAX_MOVES];
    sortMoves(pos, moves, scores, ttMove, td.killers[ply], counterMove, td.history[us]);

    Move quietsTried[MAX_MOVES];
    int quietCount = 0;

    Move bestMove = MOVE_NONE;
    int bestScore = -SCORE_INF;
    TTFlag ttFlag = TT_UPPER;

    for (int i = 0; i < moves.count; ++i) {
        Move move = moves[i];
        Piece movedPiece = pos.pieceOn(move.from());
        Piece capturedPiece = (move.flag() == FLAG_ENPASSANT) ? makePiece(~us, PAWN) : pos.pieceOn(move.to());
        bool isCapture = capturedPiece != NO_PIECE;
        bool isPromotion = move.flag() == FLAG_PROMOTION;
        bool isKiller = (move == td.killers[ply][0] || move == td.killers[ply][1]);
        bool isQuiet = !isCapture && !isPromotion;

        if (isQuiet && !pvNode && !inCheck && bestScore > -SCORE_MATE_IN_MAX_PLY) {
            if (depth <= 4 && i >= 3 + depth * depth)
                continue;
            if (depth <= 3 && staticEval != SCORE_NONE &&
                staticEval + 90 + 80 * depth <= alpha)
                continue;
        }

        if (td.useNNUE) {
            if (td.accIdx + 1 >= ThreadData::ACC_STACK_SIZE)
                break;
            pushAccumulator(td);
            applyMoveToAccumulator(td.accStack[td.accIdx], us, move, movedPiece, capturedPiece);
        }

        pos.makeMove(move, td.stateStack[ply]);
        bool givesCheck = pos.inCheck();
        td.prevMoveStack[ply] = move;

        int newDepth = depth - 1;
        int score;

        if (i == 0) {
            score = -negamax(td, -beta, -alpha, newDepth, ply + 1, true);
        } else {
            int reduction = 0;
            bool lmrOk =
                depth >= 3 &&
                !inCheck &&
                !givesCheck &&
                !isCapture &&
                !isPromotion &&
                !isKiller &&
                i >= (pvNode ? 4 : 2);

            if (lmrOk) {
                reduction = lmrTable[std::min(depth, MAX_PLY)][std::min(i, MAX_MOVES - 1)];
                if (pvNode && reduction > 0) reduction -= 1;
                reduction = std::max(0, std::min(reduction, newDepth - 1));
            }

            score = -negamax(td, -alpha - 1, -alpha, newDepth - reduction, ply + 1, true);

            if (!shared.stopped.load(std::memory_order_relaxed) &&
                reduction > 0 && score > alpha) {
                score = -negamax(td, -alpha - 1, -alpha, newDepth, ply + 1, true);
            }

            if (!shared.stopped.load(std::memory_order_relaxed) &&
                pvNode && score > alpha && score < beta) {
                score = -negamax(td, -beta, -alpha, newDepth, ply + 1, true);
            }
        }

        pos.unmakeMove(move);
        if (td.useNNUE) popAccumulator(td);

        if (shared.stopped.load(std::memory_order_relaxed))
            return 0;

        if (isQuiet && quietCount < MAX_MOVES)
            quietsTried[quietCount++] = move;

        if (score > bestScore) {
            bestScore = score;
            bestMove = move;

            if (score > alpha) {
                alpha = score;
                ttFlag = TT_EXACT;

                if (score >= beta) {
                    ttFlag = TT_LOWER;

                    if (isQuiet) {
                        if (td.killers[ply][0] != move) {
                            td.killers[ply][1] = td.killers[ply][0];
                            td.killers[ply][0] = move;
                        }

                        int bonus = historyBonus(depth);
                        updateHistory(td.history[us][move.from()][move.to()], bonus);
                        for (int q = 0; q < quietCount - 1; ++q) {
                            Move failedMove = quietsTried[q];
                            updateHistory(td.history[us][failedMove.from()][failedMove.to()], -bonus);
                        }

                        if (prevMove)
                            td.counter[us][prevMove.from()][prevMove.to()] = move;
                    }
                    break;
                }
            }
        }
    }

    if (ply == 0) {
        td.bestMove = bestMove;
        td.bestScore = bestScore;
    }

    shared.tt.store(pos.key(), scoreToTT(bestScore, ply), ttFlag, depth, bestMove, staticEval);
    return bestScore;
}

void Search::publishDepth(int depth, int score, Move bestMove, int selDepth) {
    int elapsed = elapsedMs(shared);
    int64_t nodes = shared.globalNodes.load(std::memory_order_relaxed);
    int64_t nps = elapsed > 0 ? (nodes * 1000) / elapsed : nodes;

    std::cout << "info depth " << depth
              << " seldepth " << selDepth
              << " score ";
    if (score > SCORE_MATE_IN_MAX_PLY)
        std::cout << "mate " << (SCORE_MATE - score + 1) / 2;
    else if (score < -SCORE_MATE_IN_MAX_PLY)
        std::cout << "mate -" << (SCORE_MATE + score + 1) / 2;
    else
        std::cout << "cp " << score;

    std::cout << " nodes " << nodes
              << " nps " << nps
              << " time " << elapsed;
    if (bestMove)
        std::cout << " pv " << bestMove.toUCI();
    std::cout << std::endl;
}

void Search::workerLoop(ThreadData& td) {
    for (int depth = 1; depth <= shared.limits.depth; ++depth) {
        if (shared.stopped.load(std::memory_order_relaxed))
            break;

        td.selDepth = 0;

        int alpha = -SCORE_INF;
        int beta = SCORE_INF;
        if (depth >= 5 && td.completedDepth > 0) {
            alpha = td.bestScore - 30;
            beta = td.bestScore + 30;
        }

        int score = negamax(td, alpha, beta, depth, 0, true);
        if (shared.stopped.load(std::memory_order_relaxed))
            break;

        if (score <= alpha || score >= beta) {
            score = negamax(td, -SCORE_INF, SCORE_INF, depth, 0, true);
            if (shared.stopped.load(std::memory_order_relaxed))
                break;
        }

        td.bestScore = score;
        td.completedDepth = depth;
        storeMax(shared.completedDepth, depth);

        if (!td.bestMove) {
            bool ttHit = false;
            TTEntry* entry = shared.tt.probe(td.pos.key(), ttHit);
            if (ttHit && ttMoveIsUsable(td.pos, entry->bestMove))
                td.bestMove = entry->bestMove;
        }

        if (td.threadId == 0 && printOutput_)
            publishDepth(depth, score, td.bestMove, td.selDepth);

        if (td.bestMove == td.prevBest) td.stableIters++;
        else td.stableIters = 0;
        td.prevBest = td.bestMove;

        if (td.threadId == 0 && shared.softLimit > 0) {
            int scaledSoft = shared.softLimit;
            if (td.stableIters >= 3)
                scaledSoft = (scaledSoft * 80) / 100;
            if (elapsedMs(shared) >= scaledSoft)
                break;
        }
    }

    if (td.threadId == 0)
        shared.stopped.store(true, std::memory_order_relaxed);
}

// ---------- Root ----------

void Search::go(Position& pos, const SearchLimits& limits, bool printOutput) {
    printOutput_ = printOutput;
    lastNodes_ = 0;

    shared.stopped.store(false, std::memory_order_relaxed);
    shared.globalNodes.store(0, std::memory_order_relaxed);
    shared.completedDepth.store(0, std::memory_order_relaxed);
    shared.limits = limits;
    shared.startTime = std::chrono::steady_clock::now();
    allocateTime(limits, pos.sideToMove());

    int threadCount = std::clamp(UCI::threads, 1, 256);
    bool useNNUE = UCI::useNNUE && NNUE::isLoaded();

    std::vector<ThreadData> threadData(threadCount);
    for (int i = 0; i < threadCount; ++i) {
        initThreadData(threadData[i], pos, useNNUE);
        threadData[i].threadId = i;
    }

    std::vector<std::thread> helpers;
    helpers.reserve(std::max(0, threadCount - 1));
    for (int i = 1; i < threadCount; ++i)
        helpers.emplace_back(&Search::workerLoop, this, std::ref(threadData[i]));

    workerLoop(threadData[0]);
    shared.stopped.store(true, std::memory_order_relaxed);

    for (auto& worker : helpers)
        worker.join();

    lastNodes_ = shared.globalNodes.load(std::memory_order_relaxed);

    Move bestMove = threadData[0].bestMove;
    if (!bestMove) {
        MoveList moves;
        MoveGen::generateLegal(pos, moves);
        if (moves.count > 0)
            bestMove = moves[0];
    }

    if (printOutput_) {
        if (bestMove)
            std::cout << "bestmove " << bestMove.toUCI() << std::endl;
        else
            std::cout << "bestmove 0000" << std::endl;
    }
}
