#include "search.h"
#include "eval.h"
#include "movegen.h"
#include "policy.h"
#include "see.h"
#include "uci.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

// int16_t-typed variant for the continuation- and capture-history tables.
// Same gravity formula; explicit clamp to int16_t range to avoid overflow.
static inline void updateHistory16(int16_t& entry, int bonus) {
    int e = entry;
    e += bonus - e * std::abs(bonus) / HISTORY_MAX;
    if (e >  HISTORY_MAX) e =  HISTORY_MAX;
    if (e < -HISTORY_MAX) e = -HISTORY_MAX;
    entry = int16_t(e);
}

// (V7) Extended move scorer. ContHist contributes to quiet ordering;
// captHist (scaled) contributes to capture ordering. MVV-LVA stays as the
// primary key for captures via a *100 multiplier so different
// victim/attacker ratios always sort correctly regardless of history.
static int scoreMove(const Position& pos, Move move, Move ttMove,
                     const Move killers[2], Move counterMove,
                     const int history[64][64],
                     const int16_t (*contHist)[64][13][64],
                     const int16_t (*captHist)[64][7],
                     int prevPIdx, Square prevTo,
                     int policyBonus, bool quietResidual) {
    if (move == ttMove) return 1000000;

    Square to = move.to();
    Piece captured = pos.pieceOn(to);
    if (move.flag() == FLAG_ENPASSANT)
        captured = makePiece(~pos.sideToMove(), PAWN);

    // In quiet_residual mode the policy bonus is suppressed for the
    // tactical (capture / promotion) branches so that the model cannot
    // perturb cross-bucket ordering. Killers / counters / history are
    // quiet by definition and still receive the residual.
    int captureBonus   = quietResidual ? 0 : policyBonus;
    int promotionBonus = quietResidual ? 0 : policyBonus;

    Piece mover = pos.pieceOn(move.from());
    int moverIdx = pieceIdx(mover);

    if (captured != NO_PIECE) {
        // (V7) MVV-LVA * 100 keeps the victim/attacker ordering dominant;
        // captHist breaks ties with up to ±16K.
        int mvv = MVV_LVA[pieceType(mover)][pieceType(captured)];
        int ch = int(captHist[moverIdx][to][pieceType(captured)]);
        return 100000 + mvv * 100 + ch + captureBonus;
    }

    if (move.flag() == FLAG_PROMOTION) return 90000 + move.promoPiece() + promotionBonus;
    if (move == killers[0]) return 80000 + policyBonus;
    if (move == killers[1]) return 79000 + policyBonus;
    if (counterMove && move == counterMove) return 78000 + policyBonus;

    int hist = history[move.from()][to];
    int cont = int(contHist[prevPIdx][prevTo][moverIdx][to]);
    return hist + cont + policyBonus;
}

static void sortMoves(const Position& pos, MoveList& moves, int* scores,
                      Move ttMove, const Move killers[2], Move counterMove,
                      const int history[64][64],
                      const int16_t (*contHist)[64][13][64],
                      const int16_t (*captHist)[64][7],
                      int prevPIdx, Square prevTo,
                      const int* policyBonuses = nullptr,
                      bool quietResidual = false) {
    for (int i = 0; i < moves.count; ++i)
        scores[i] = scoreMove(pos, moves[i], ttMove, killers, counterMove, history,
                              contHist, captHist, prevPIdx, prevTo,
                              policyBonuses ? policyBonuses[i] : 0,
                              quietResidual);

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

// Capture-only ordering for qsearch and ProbCut. TT move (when given) jumps
// to the front; otherwise sorted by MVV-LVA + captHist tie-breaker.
static int scoreCaptureMove(const Position& pos, Move move,
                            const int16_t (*captHist)[64][7],
                            Move ttMove) {
    if (move == ttMove) return 1000000;

    Piece captured = pos.pieceOn(move.to());
    if (move.flag() == FLAG_ENPASSANT)
        captured = makePiece(~pos.sideToMove(), PAWN);
    if (captured == NO_PIECE) {
        // Quiet promotions reach this path from generateCaptures.
        if (move.flag() == FLAG_PROMOTION)
            return 50000 + move.promoPiece();
        return 0;
    }

    Piece mover = pos.pieceOn(move.from());
    int mvv = MVV_LVA[pieceType(mover)][pieceType(captured)];
    int ch = int(captHist[pieceIdx(mover)][move.to()][pieceType(captured)]);
    return 100000 + mvv * 100 + ch;
}

static void sortCaptures(const Position& pos, MoveList& moves,
                         const int16_t (*captHist)[64][7],
                         Move ttMove = MOVE_NONE) {
    int scores[MAX_MOVES];
    for (int i = 0; i < moves.count; ++i)
        scores[i] = scoreCaptureMove(pos, moves[i], captHist, ttMove);

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

// (V7) Lazy-SMP skip pattern. Helper threads skip iterative-deepening
// iterations according to (depth+phase) / size parity. Decorrelates the
// search trees explored by the helpers without changing the main thread.
static constexpr int kSkipSize[]  = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4};
static constexpr int kSkipPhase[] = {0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 6, 7};
static constexpr int kSkipPatternSize = int(sizeof(kSkipSize) / sizeof(kSkipSize[0]));

static inline bool helperShouldSkip(int threadId, int depth) {
    if (threadId == 0) return false;
    int idx = (threadId - 1) % kSkipPatternSize;
    int s = kSkipSize[idx];
    int p = kSkipPhase[idx];
    return ((depth + p) / s) % 2 != 0;
}

namespace {

inline int elapsedMs(const SearchShared& shared) {
    auto now = std::chrono::steady_clock::now();
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(now - shared.startTime).count());
}

inline int searchOverheadMs(int budgetMs) {
    if (budgetMs <= 20)
        return 1;
    return std::clamp(budgetMs / 20, 2, 25);
}

inline int effectiveMoveOverheadMs(int budgetMs) {
    return std::max(searchOverheadMs(budgetMs), std::clamp(UCI::moveOverhead, 0, 5000));
}

inline void storeMax(std::atomic<int>& target, int value) {
    int current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

inline bool rootMoveAllowed(const SearchLimits& limits, Move move) {
    if (limits.searchMoveCount <= 0)
        return true;
    for (int i = 0; i < limits.searchMoveCount; ++i) {
        if (limits.searchMoves[i] == move)
            return true;
    }
    return false;
}

void filterRootMoves(MoveList& moves, const SearchLimits& limits) {
    if (limits.searchMoveCount <= 0)
        return;

    int out = 0;
    for (int i = 0; i < moves.count; ++i) {
        if (rootMoveAllowed(limits, moves[i]))
            moves[out++] = moves[i];
    }
    moves.count = out;
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

    if (td.threadId == 0 && (global & 255) == 0)
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
        int overhead = std::min(effectiveMoveOverheadMs(limits.movetime),
                                std::max(0, limits.movetime - 1));
        int budget = std::max(1, limits.movetime - overhead);
        shared.softLimit = budget;
        shared.hardLimit = budget;
        return;
    }
    if (limits.infinite)
        return;

    int timeLeft = (side == WHITE) ? limits.wtime : limits.btime;
    int inc = (side == WHITE) ? limits.winc : limits.binc;
    if (timeLeft <= 0)
        return;

    int movesToGo = limits.movestogo > 0 ? limits.movestogo : 30;
    int reserve = std::max(20, timeLeft / 50);
    int usable = std::max(1, timeLeft - reserve);
    int soft = usable / movesToGo + inc * 3 / 4;
    soft = std::max(5, soft);
    soft = std::min(soft, std::max(1, usable / 2));

    int hard = soft * 4;
    int cap = std::max(5, usable / 3);
    hard = std::min(hard, cap);
    hard = std::max(hard, soft);
    int overhead = std::min(effectiveMoveOverheadMs(hard), std::max(0, hard - 1));
    hard = std::max(1, hard - overhead);
    soft = std::min(soft, hard);

    shared.softLimit = soft;
    shared.hardLimit = hard;
}

void Search::initThreadData(ThreadData& td, const Position& root) {
    td = ThreadData{};
    td.pos.copyFrom(root, td.rootState);
}

// ---------- Quiescence ----------

int Search::quiescence(ThreadData& td, int alpha, int beta, int ply) {
    Position& pos = td.pos;

    if (shared.stopped.load(std::memory_order_relaxed))
        return 0;
    if (ply >= ThreadData::STATE_STACK_SIZE - 1)
        return Eval::evaluate(pos);
    if (onNode(td, ply))
        return 0;

    bool pvNode = (beta - alpha > 1);
    int origAlpha = alpha;

    // (V7) TT probe in qsearch. qsearch entries are stored with depth=0;
    // any TT hit can satisfy a non-PV cutoff because qsearch depth is the
    // minimum search depth.
    bool ttHit = false;
    TTEntry* ttEntry = shared.tt.probe(pos.key(), ttHit);
    Move ttMove = MOVE_NONE;
    int ttScore = SCORE_NONE;
    int ttStaticEval = SCORE_NONE;
    TTFlag ttFlag = TT_NONE;
    if (ttHit) {
        ttMove = ttEntry->bestMove;
        if (!ttMoveIsUsable(pos, ttMove)) {
            ttHit = false;
            ttMove = MOVE_NONE;
        } else {
            ttScore = scoreFromTT(ttEntry->score, ply);
            ttStaticEval = ttEntry->staticEval;
            ttFlag = ttEntry->flag();
            if (!pvNode) {
                if (ttFlag == TT_EXACT) return ttScore;
                if (ttFlag == TT_LOWER && ttScore >= beta) return ttScore;
                if (ttFlag == TT_UPPER && ttScore <= alpha) return ttScore;
            }
        }
    }

    int standPat;
    if (ttHit && ttStaticEval != SCORE_NONE) {
        standPat = ttStaticEval;
    } else {
        standPat = Eval::evaluate(pos);
    }
    // Refine stand-pat with TT score when its bound is tighter — same trick
    // used in the main search.
    int eval = standPat;
    if (ttHit) {
        if (ttFlag == TT_EXACT ||
            (ttFlag == TT_LOWER && ttScore > eval) ||
            (ttFlag == TT_UPPER && ttScore < eval))
            eval = ttScore;
    }

    if (eval >= beta) {
        if (!ttHit)
            shared.tt.store(pos.key(), scoreToTT(eval, ply), TT_LOWER, 0, MOVE_NONE, standPat);
        return eval;
    }
    if (alpha < eval) alpha = eval;

    const int DELTA = 1000;
    if (eval + DELTA < alpha) return alpha;

    MoveList captures;
    MoveGen::generateCaptures(pos, captures);
    sortCaptures(pos, captures, td.captHist, ttMove);

    Color us = pos.sideToMove();
    Move bestMove = MOVE_NONE;
    int bestScore = eval;

    for (int i = 0; i < captures.count; ++i) {
        Move move = captures[i];
        Piece capturedPiece = (move.flag() == FLAG_ENPASSANT) ? makePiece(~us, PAWN) : pos.pieceOn(move.to());

        // SEE pruning: when stand-pat + captured-value can't reach alpha, skip
        // captures that lose more than 50cp by static exchange.
        int capForPrune = (capturedPiece != NO_PIECE) ? SEE::PieceValue[pieceType(capturedPiece)] : 0;
        if (eval + capForPrune + 200 <= alpha &&
            !SEE::seeGE(pos, move, -50))
            continue;

        pos.makeMove(move, td.stateStack[ply]);
        shared.tt.prefetch(pos.key());
        if (pos.isSquareAttacked(pos.kingSq(us), pos.sideToMove())) {
            pos.unmakeMove(move);
            continue;
        }

        int score = -quiescence(td, -beta, -alpha, ply + 1);

        pos.unmakeMove(move);

        if (shared.stopped.load(std::memory_order_relaxed))
            return 0;
        if (score > bestScore) {
            bestScore = score;
            bestMove = move;
            if (score > alpha) {
                alpha = score;
                if (score >= beta)
                    break;
            }
        }
    }

    // (V7) Store the qsearch result so subsequent visits can cut.
    TTFlag storeFlag = (bestScore >= beta) ? TT_LOWER
                      : (bestScore > origAlpha) ? TT_EXACT : TT_UPPER;
    shared.tt.store(pos.key(), scoreToTT(bestScore, ply), storeFlag, 0, bestMove, standPat);

    return bestScore;
}

// ---------- Main negamax ----------

int Search::negamax(ThreadData& td, int alpha, int beta, int depth, int ply, bool doNull, bool cutNode) {
    Position& pos = td.pos;

    if (shared.stopped.load(std::memory_order_relaxed))
        return 0;
    if (ply >= MAX_PLY - 1)
        return Eval::evaluate(pos);
    if (ply >= ThreadData::STATE_STACK_SIZE - 1)
        return Eval::evaluate(pos);

    Move excludedMove = td.excludedMoveStack[ply];
    bool isExcluded = (excludedMove != MOVE_NONE);

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
    int ttScore = SCORE_NONE;
    int ttStaticEval = SCORE_NONE;
    int ttDepth = 0;
    TTFlag ttFlag = TT_NONE;
    if (ttHit) {
        ttMove = ttEntry->bestMove;
        if (!ttMoveIsUsable(pos, ttMove)) {
            ttHit = false;
            ttMove = MOVE_NONE;
        } else {
            ttScore = scoreFromTT(ttEntry->score, ply);
            ttStaticEval = ttEntry->staticEval;
            ttDepth = ttEntry->depth;
            ttFlag = ttEntry->flag();
            // Suppress TT cutoff while in a singular sub-search — the same
            // position may legitimately fail-high without the TT move.
            if (!isExcluded && !pvNode && ttDepth >= depth) {
                if (ttFlag == TT_EXACT) return ttScore;
                if (ttFlag == TT_LOWER && ttScore >= beta) return ttScore;
                if (ttFlag == TT_UPPER && ttScore <= alpha) return ttScore;
            }
        }
    }

    // (V7) Compute raw static eval + a "refined" eval that incorporates the
    // tighter bound the TT score may give us (drives RFP / razor / NMP).
    int staticEval, eval;
    if (inCheck) {
        staticEval = SCORE_NONE;
        eval = SCORE_NONE;
    } else if (ttHit && ttStaticEval != SCORE_NONE) {
        staticEval = ttStaticEval;
        eval = staticEval;
        if (ttFlag == TT_EXACT ||
            (ttFlag == TT_LOWER && ttScore > eval) ||
            (ttFlag == TT_UPPER && ttScore < eval))
            eval = ttScore;
    } else {
        staticEval = Eval::evaluate(pos);
        eval = staticEval;
    }
    td.staticEvalStack[ply] = staticEval;

    // Improving: side-to-move's static eval increased vs. two plies ago.
    bool improving = false;
    int improvement = 0;
    if (!inCheck && ply >= 2 && staticEval != SCORE_NONE) {
        int prev = td.staticEvalStack[ply - 2];
        if (prev != SCORE_NONE) {
            improving = staticEval > prev;
            improvement = staticEval - prev;
        }
    }
    int clampedImprovement = std::clamp(improvement, -200, 200);

    bool prunable = !pvNode && !inCheck && !isExcluded && eval != SCORE_NONE;

    // RFP (static null move).
    if (prunable && depth <= 7) {
        int margin = (improving ? 55 : 75) * depth - clampedImprovement * 2;
        if (eval - margin >= beta)
            return eval;
    }

    // Razoring.
    if (prunable && depth <= 4) {
        int razorMargin = 350 + 300 * depth;
        if (eval + razorMargin < alpha) {
            int razorScore = quiescence(td, alpha - 1, alpha, ply);
            if (razorScore < alpha)
                return razorScore;
        }
    }

    // Null-move pruning. Eval-based bump on R (up to +3).
    if (doNull && !inCheck && !pvNode && !isExcluded && depth >= 3 &&
        eval != SCORE_NONE && eval >= beta) {
        Bitboard nonPawnMaterial = pos.pieces(pos.sideToMove()) &
            ~pos.pieces(pos.sideToMove(), PAWN) &
            ~pos.pieces(pos.sideToMove(), KING);
        if (nonPawnMaterial) {
            int evalBump = std::min((eval - beta) / 200, 3);
            int R = 3 + depth / 3 + evalBump;

            td.prevMoveStack[ply] = MOVE_NONE;
            td.movedPieceStack[ply] = NO_PIECE;
            pos.doNullMove(td.stateStack[ply]);
            shared.tt.prefetch(pos.key());
            int nullScore = -negamax(td, -beta, -beta + 1, depth - R - 1, ply + 1, false, !cutNode);
            pos.undoNullMove();

            if (shared.stopped.load(std::memory_order_relaxed))
                return 0;
            if (nullScore >= beta) {
                // Avoid claiming faked mate via null pruning.
                if (nullScore >= SCORE_MATE_IN_MAX_PLY)
                    nullScore = beta;
                return nullScore;
            }
        }
    }

    // (V7) ProbCut. If we have a likely fail-high, search good captures to
    // verify quickly before doing the full move loop.
    if (!pvNode && !inCheck && !isExcluded && depth >= 5 &&
        eval != SCORE_NONE && std::abs(beta) < SCORE_MATE_IN_MAX_PLY) {
        int probcutBeta = beta + 200;
        if (eval + 100 >= probcutBeta) {
            MoveList caps;
            MoveGen::generateCaptures(pos, caps);
            sortCaptures(pos, caps, td.captHist, MOVE_NONE);

            Color usPC = pos.sideToMove();
            for (int i = 0; i < caps.count; ++i) {
                Move m = caps[i];
                if (m == ttMove) continue;
                Piece capturedPiece = (m.flag() == FLAG_ENPASSANT)
                    ? makePiece(~usPC, PAWN) : pos.pieceOn(m.to());
                bool isCap = (capturedPiece != NO_PIECE);
                bool isPromo = (m.flag() == FLAG_PROMOTION);
                if (!isCap && !isPromo) continue;
                if (!SEE::seeGE(pos, m, probcutBeta - eval)) continue;

                Piece moverP = pos.pieceOn(m.from());
                td.prevMoveStack[ply] = m;
                td.movedPieceStack[ply] = moverP;
                pos.makeMove(m, td.stateStack[ply]);
                shared.tt.prefetch(pos.key());
                if (pos.isSquareAttacked(pos.kingSq(usPC), pos.sideToMove())) {
                    pos.unmakeMove(m);
                    continue;
                }
                int score = -quiescence(td, -probcutBeta, -probcutBeta + 1, ply + 1);
                if (score >= probcutBeta) {
                    score = -negamax(td, -probcutBeta, -probcutBeta + 1,
                                     depth - 4, ply + 1, true, !cutNode);
                }
                pos.unmakeMove(m);
                if (shared.stopped.load(std::memory_order_relaxed))
                    return 0;
                if (score >= probcutBeta) {
                    shared.tt.store(pos.key(), scoreToTT(score, ply), TT_LOWER,
                                    depth - 3, m, staticEval);
                    return score;
                }
            }
        }
    }

    // Internal Iterative Reduction.
    if ((pvNode || cutNode) && depth >= 4 && ttMove == MOVE_NONE)
        depth--;

    MoveList moves;
    MoveGen::generateLegal(pos, moves);
    if (moves.count == 0) {
        // No legal moves — for a singular search this means the TT move is
        // the only move; return alpha (the test failed low → not singular).
        if (isExcluded) return alpha;
        return inCheck ? (-SCORE_MATE + ply) : SCORE_DRAW;
    }
    if (ply == 0) {
        filterRootMoves(moves, shared.limits);
        if (moves.count == 0)
            return SCORE_DRAW;
    }

    Move prevMove = (ply > 0) ? td.prevMoveStack[ply - 1] : MOVE_NONE;
    Piece prevPiece = (ply > 0) ? td.movedPieceStack[ply - 1] : NO_PIECE;
    Square prevTo = prevMove ? prevMove.to() : Square(0);
    int prevPIdx = pieceIdx(prevPiece);

    Color us = pos.sideToMove();
    Move counterMove = MOVE_NONE;
    if (prevMove)
        counterMove = td.counter[us][prevMove.from()][prevMove.to()];

    int scores[MAX_MOVES];
    int policyBonuses[MAX_MOVES] = {};
    const int* rootPolicyBonuses = nullptr;
    if (ply == 0 && shared.rootPolicyValid) {
        bool ordered = (moves.count == shared.rootPolicyCount);
        for (int i = 0; ordered && i < moves.count; ++i)
            if (moves[i].data != shared.rootPolicyMoves[i].data) ordered = false;
        if (ordered) {
            std::memcpy(policyBonuses, shared.rootPolicyBonus,
                        size_t(moves.count) * sizeof(int));
        } else {
            for (int i = 0; i < moves.count; ++i) {
                int b = 0;
                for (int j = 0; j < shared.rootPolicyCount; ++j) {
                    if (shared.rootPolicyMoves[j].data == moves[i].data) {
                        b = shared.rootPolicyBonus[j];
                        break;
                    }
                }
                policyBonuses[i] = b;
            }
        }
        rootPolicyBonuses = policyBonuses;
    }
    bool quietResidual = (ply == 0) &&
        ((UCI::usePolicyV2 && UCI::policyV2Mode == 2) ||
         (!UCI::usePolicyV2 && UCI::policyMode == 2));
    sortMoves(pos, moves, scores, ttMove, td.killers[ply], counterMove,
              td.history[us], td.contHist, td.captHist,
              prevPIdx, prevTo, rootPolicyBonuses, quietResidual);

    // (V7) Singular Extension probe — done once before the main loop.
    int singularExtend = 0;
    Move singularTtMove = MOVE_NONE;
    if (!isExcluded && ttMove != MOVE_NONE && depth >= 8 &&
        ttDepth >= depth - 3 &&
        (ttFlag == TT_LOWER || ttFlag == TT_EXACT) &&
        std::abs(ttScore) < SCORE_MATE_IN_MAX_PLY) {
        int singularBeta = ttScore - depth * 2;
        td.excludedMoveStack[ply] = ttMove;
        int singResult = negamax(td, singularBeta - 1, singularBeta,
                                 (depth - 1) / 2, ply, false, cutNode);
        td.excludedMoveStack[ply] = MOVE_NONE;
        if (shared.stopped.load(std::memory_order_relaxed))
            return 0;
        if (singResult < singularBeta) {
            singularExtend = 1;
            singularTtMove = ttMove;
        } else if (singularBeta >= beta) {
            // Multi-cut: TT move's bound + another move both ≥ beta.
            return singularBeta;
        }
    }

    Move quietsTried[MAX_MOVES];
    Piece quietPieceTried[MAX_MOVES];
    int quietCount = 0;
    Move capturesTried[MAX_MOVES];
    Piece capturePieceTried[MAX_MOVES];
    PieceType captureCapturedTried[MAX_MOVES];
    int captureCount = 0;

    Move bestMove = MOVE_NONE;
    int bestScore = -SCORE_INF;
    TTFlag ttStoreFlag = TT_UPPER;

    for (int i = 0; i < moves.count; ++i) {
        if (ply == 0)
            checkTime();
        if (shared.stopped.load(std::memory_order_relaxed))
            return 0;

        Move move = moves[i];
        if (move == excludedMove) continue;

        Piece moverPiece = pos.pieceOn(move.from());
        int moverIdx = pieceIdx(moverPiece);
        Piece capturedPiece = (move.flag() == FLAG_ENPASSANT) ? makePiece(~us, PAWN) : pos.pieceOn(move.to());
        bool isCapture = capturedPiece != NO_PIECE;
        bool isPromotion = move.flag() == FLAG_PROMOTION;
        bool isKiller = (move == td.killers[ply][0] || move == td.killers[ply][1]);
        bool isQuiet = !isCapture && !isPromotion;

        if (isQuiet && !pvNode && !inCheck && bestScore > -SCORE_MATE_IN_MAX_PLY) {
            int lmpThreshold = (improving ? 4 : 2) + depth * depth;
            if (depth <= 4 && i >= lmpThreshold)
                continue;
            if (depth <= 3 && eval != SCORE_NONE &&
                eval + 90 + (improving ? 60 : 80) * depth <= alpha)
                continue;
            if (depth <= 7 && !SEE::seeGE(pos, move, -60 * depth))
                continue;
        }

        if (isCapture && !pvNode && !inCheck && depth <= 8 &&
            bestScore > -SCORE_MATE_IN_MAX_PLY &&
            !SEE::seeGE(pos, move, -30 * depth))
            continue;

        int64_t nodesBefore = (ply == 0) ? shared.globalNodes.load(std::memory_order_relaxed) : 0;

        // Record move context for the child node's contHist lookup BEFORE
        // makeMove (so pos.pieceOn(from) still returns the moving piece).
        td.prevMoveStack[ply] = move;
        td.movedPieceStack[ply] = moverPiece;

        pos.makeMove(move, td.stateStack[ply]);
        shared.tt.prefetch(pos.key());
        bool givesCheck = pos.inCheck();

        // SE extension applies only to the TT move at this node.
        int extension = (singularExtend && move == singularTtMove) ? 1 : 0;
        int newDepth = depth - 1 + extension;
        int score;

        if (i == 0) {
            score = -negamax(td, -beta, -alpha, newDepth, ply + 1, true,
                             pvNode ? false : !cutNode);
        } else {
            int reduction = 0;
            bool reduceCapture = isCapture && !SEE::seeGE(pos, move, 0);
            bool lmrOk =
                depth >= 3 &&
                !inCheck &&
                !givesCheck &&
                !isPromotion &&
                (isQuiet || reduceCapture) &&
                i >= (pvNode ? 4 : 2);

            if (lmrOk) {
                reduction = lmrTable[std::min(depth, MAX_PLY)][std::min(i, MAX_MOVES - 1)];
                if (pvNode && reduction > 0) reduction -= 1;
                if (!improving) reduction += 1;
                if (cutNode && !pvNode) reduction += 1;
                if (isKiller && reduction > 0) reduction -= 1;

                if (isQuiet) {
                    int hist = td.history[us][move.from()][move.to()] +
                               int(td.contHist[prevPIdx][prevTo][moverIdx][move.to()]);
                    reduction -= std::clamp(hist / 8192, -2, 2);
                } else if (reduceCapture) {
                    // Less aggressive on captures.
                    reduction = (reduction + 1) / 2;
                    int chist = int(td.captHist[moverIdx][move.to()][pieceType(capturedPiece)]);
                    reduction -= std::clamp(chist / 8192, -2, 2);
                }

                reduction = std::max(0, std::min(reduction, newDepth - 1));
            }

            score = -negamax(td, -alpha - 1, -alpha, newDepth - reduction, ply + 1, true, !cutNode);

            if (!shared.stopped.load(std::memory_order_relaxed) &&
                reduction > 0 && score > alpha) {
                score = -negamax(td, -alpha - 1, -alpha, newDepth, ply + 1, true, !cutNode);
            }

            if (!shared.stopped.load(std::memory_order_relaxed) &&
                pvNode && score > alpha && score < beta) {
                score = -negamax(td, -beta, -alpha, newDepth, ply + 1, true, false);
            }
        }

        pos.unmakeMove(move);

        if (shared.stopped.load(std::memory_order_relaxed))
            return 0;

        if (ply == 0) {
            int64_t nodesAfter = shared.globalNodes.load(std::memory_order_relaxed);
            int64_t spent = nodesAfter - nodesBefore;
            if (spent < 0) spent = 0;
            if (i < MAX_MOVES)
                td.rootMoveNodes[i] += spent;
        }

        if (isQuiet && quietCount < MAX_MOVES) {
            quietsTried[quietCount] = move;
            quietPieceTried[quietCount] = moverPiece;
            quietCount++;
        }
        if (isCapture && captureCount < MAX_MOVES) {
            capturesTried[captureCount] = move;
            capturePieceTried[captureCount] = moverPiece;
            captureCapturedTried[captureCount] = pieceType(capturedPiece);
            captureCount++;
        }

        if (score > bestScore) {
            bestScore = score;
            bestMove = move;
            if (ply == 0) td.bestRootIdx = i;

            if (score > alpha) {
                alpha = score;
                ttStoreFlag = TT_EXACT;

                if (score >= beta) {
                    ttStoreFlag = TT_LOWER;

                    int bonus = historyBonus(depth);

                    if (isQuiet) {
                        if (td.killers[ply][0] != move) {
                            td.killers[ply][1] = td.killers[ply][0];
                            td.killers[ply][0] = move;
                        }

                        updateHistory(td.history[us][move.from()][move.to()], bonus);
                        updateHistory16(td.contHist[prevPIdx][prevTo][moverIdx][move.to()], bonus);
                        for (int q = 0; q < quietCount - 1; ++q) {
                            Move qm = quietsTried[q];
                            Piece qp = quietPieceTried[q];
                            updateHistory(td.history[us][qm.from()][qm.to()], -bonus);
                            updateHistory16(td.contHist[prevPIdx][prevTo][pieceIdx(qp)][qm.to()], -bonus);
                        }

                        if (prevMove)
                            td.counter[us][prevMove.from()][prevMove.to()] = move;
                    } else if (isCapture) {
                        updateHistory16(td.captHist[moverIdx][move.to()][pieceType(capturedPiece)], bonus);
                    }

                    // Capture malus: penalise earlier captures that didn't cut.
                    for (int q = 0; q < captureCount - 1; ++q) {
                        Move cm = capturesTried[q];
                        Piece cp = capturePieceTried[q];
                        PieceType cv = captureCapturedTried[q];
                        updateHistory16(td.captHist[pieceIdx(cp)][cm.to()][cv], -bonus);
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

    // Don't poison the TT with a singular sub-search result.
    if (!isExcluded && !(ply == 0 && shared.limits.searchMoveCount > 0))
        shared.tt.store(pos.key(), scoreToTT(bestScore, ply), ttStoreFlag, depth, bestMove, staticEval);
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

        // (V7) Helper threads skip selected iterations to diversify search.
        if (helperShouldSkip(td.threadId, depth))
            continue;

        td.selDepth = 0;
        std::memset(td.rootMoveNodes, 0, sizeof(td.rootMoveNodes));

        int alpha = -SCORE_INF;
        int beta = SCORE_INF;
        int delta = 10;
        if (depth >= 5 && td.completedDepth > 0) {
            alpha = std::max(-SCORE_INF, td.bestScore - delta);
            beta  = std::min( SCORE_INF, td.bestScore + delta);
        }

        // Graduated aspiration widening.
        int score;
        while (true) {
            score = negamax(td, alpha, beta, depth, 0, true, false);
            if (shared.stopped.load(std::memory_order_relaxed))
                break;

            if (score <= alpha) {
                beta = (alpha + beta) / 2;
                alpha = std::max(-SCORE_INF, alpha - delta);
            } else if (score >= beta) {
                beta = std::min(SCORE_INF, beta + delta);
            } else {
                break;
            }
            delta += delta / 2;
        }
        if (shared.stopped.load(std::memory_order_relaxed))
            break;

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
        else {
            td.stableIters = 0;
            td.bestMoveChanges += 1;
        }
        td.prevBest = td.bestMove;

        if (td.threadId == 0 && shared.softLimit > 0) {
            int scaledSoft = shared.softLimit;
            if (td.stableIters >= 3)
                scaledSoft = (scaledSoft * 80) / 100;

            // (V7) Best-move-change extension and best-move node-share scale.
            // These are only used past a minimum depth so the early
            // iterations don't budget for noise.
            if (depth >= 6) {
                int bmcCap = std::min(td.bestMoveChanges, 4);
                int bmcBonusPerMille = bmcCap * 150; // +15% per change, capped +60%
                int64_t nodesTotal = 1;
                for (int i = 0; i < MAX_MOVES; ++i)
                    if (td.rootMoveNodes[i] > 0) nodesTotal += td.rootMoveNodes[i];
                int64_t bestNodes = (td.bestRootIdx >= 0 && td.bestRootIdx < MAX_MOVES)
                    ? td.rootMoveNodes[td.bestRootIdx] : 0;
                int shareMille = int((bestNodes * 1000) / nodesTotal);
                // factor: 1.5 - 1.0 * share → [0.5, 1.5]
                int shareScale = std::clamp(1500 - shareMille, 500, 1500);

                int scale = 1000 + bmcBonusPerMille;
                if (scale > 1600) scale = 1600;
                scaledSoft = (scaledSoft * scale) / 1000;
                scaledSoft = (scaledSoft * shareScale) / 1000;

                td.bestMoveChanges = td.bestMoveChanges / 2;
            }

            // Confidence-driven shave from policy-time-mod (legacy).
            if (UCI::policyTimeMod && shared.rootPolicyValid &&
                shared.policyConfidence > 0 && UCI::policyTimeBoost > 0) {
                int margin = std::min(shared.policyConfidence, UCI::policyTimeMargin);
                int marginNorm = (margin * 1000) / UCI::policyTimeMargin;
                int shavePerMille = (UCI::policyTimeBoost * marginNorm) / 100;
                scaledSoft = (scaledSoft * (1000 - shavePerMille)) / 1000;
            }
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
    lastBestMove_ = MOVE_NONE;

    shared.stopped.store(false, std::memory_order_relaxed);
    shared.globalNodes.store(0, std::memory_order_relaxed);
    shared.completedDepth.store(0, std::memory_order_relaxed);
    shared.limits = limits;
    shared.startTime = std::chrono::steady_clock::now();
    shared.tt.newSearch();
    allocateTime(limits, pos.sideToMove());

    int threadCount = std::clamp(UCI::threads, 1, 256);

    if (limits.searchMoveCount > 0) {
        MoveList legalRootMoves;
        MoveGen::generateLegal(pos, legalRootMoves);
        filterRootMoves(legalRootMoves, limits);
        if (legalRootMoves.count == 0) {
            lastNodes_ = 0;
            lastBestMove_ = MOVE_NONE;
            if (printOutput_)
                std::cout << "bestmove 0000" << std::endl;
            return;
        }
    }

    // Populate the root policy cache once. Reused across all ID iterations
    // and aspiration re-searches by every worker thread.
    shared.rootPolicyCount = 0;
    shared.rootPolicyValid = false;
    shared.policyConfidence = 0;
    bool wantV1 = UCI::usePolicy   && UCI::policyMode   != 0 && Policy::isLoaded()   && UCI::policyScale != 0;
    bool wantV2 = UCI::usePolicyV2 && UCI::policyV2Mode != 0 && Policy::isV2Loaded();
    if (wantV1 || wantV2) {
        MoveList rootMoves;
        MoveGen::generateLegal(pos, rootMoves);
        filterRootMoves(rootMoves, limits);
        if (wantV2) {
            Policy::scoreMovesV2(pos, rootMoves.moves, rootMoves.count,
                                 UCI::policyV2Scale, UCI::policyV2QuietScale,
                                 UCI::policyV2EndgameScale,
                                 UCI::policyV2BonusClamp,
                                 shared.rootPolicyBonus);
        } else {
            Policy::scoreMoves(pos, rootMoves.moves, rootMoves.count,
                               UCI::policyScale, shared.rootPolicyBonus);
        }
        for (int i = 0; i < rootMoves.count; ++i)
            shared.rootPolicyMoves[i] = rootMoves[i];
        shared.rootPolicyCount = rootMoves.count;
        shared.rootPolicyValid = true;

        // top1 - top2 margin over the cached bonuses.
        if (rootMoves.count >= 2) {
            int top1 = INT_MIN;
            int top2 = INT_MIN;
            for (int i = 0; i < rootMoves.count; ++i) {
                int b = shared.rootPolicyBonus[i];
                if (b > top1) { top2 = top1; top1 = b; }
                else if (b > top2) { top2 = b; }
            }
            shared.policyConfidence = top1 - top2;
        }
    }

    std::vector<ThreadData> threadData(threadCount);
    for (int i = 0; i < threadCount; ++i) {
        initThreadData(threadData[i], pos);
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
    lastBestMove_ = bestMove;

    if (printOutput_) {
        if (bestMove)
            std::cout << "bestmove " << bestMove.toUCI() << std::endl;
        else
            std::cout << "bestmove 0000" << std::endl;
    }
}
