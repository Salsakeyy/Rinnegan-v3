#include "search.h"
#include "eval.h"
#include "movegen.h"
#include <iostream>
#include <algorithm>
#include <cmath>

// ---------- Move ordering ----------

// MVV-LVA scoring for captures.
static const int MVV_LVA[7][7] = {
    // victim:  P    N    B    R    Q    K   None
    /* P */  { 105, 205, 305, 405, 505, 605, 0 },
    /* N */  { 104, 204, 304, 404, 504, 604, 0 },
    /* B */  { 103, 203, 303, 403, 503, 603, 0 },
    /* R */  { 102, 202, 302, 402, 502, 602, 0 },
    /* Q */  { 101, 201, 301, 401, 501, 601, 0 },
    /* K */  { 100, 200, 300, 400, 500, 600, 0 },
    /* No */ {   0,   0,   0,   0,   0,   0, 0 },
};

// History gravity cap - keeps bonuses bounded.
static constexpr int HISTORY_MAX = 16384;

static inline int historyBonus(int depth) {
    int b = depth * depth;
    if (b > 400) b = 400;
    return b;
}

// Gravity update: prevents unbounded growth, amplifies moves that consistently succeed.
static inline void updateHistory(int& entry, int bonus) {
    entry += bonus - entry * std::abs(bonus) / HISTORY_MAX;
}

static int scoreMove(const Position& pos, Move m, Move ttMove,
                     const Move killers[2], Move counterMove,
                     const int history[64][64]) {
    if (m == ttMove) return 1000000;

    Square to = m.to();
    Piece captured = pos.pieceOn(to);
    if (m.flag() == FLAG_ENPASSANT) captured = makePiece(~pos.sideToMove(), PAWN);

    if (captured != NO_PIECE) {
        Piece mover = pos.pieceOn(m.from());
        return 100000 + MVV_LVA[pieceType(mover)][pieceType(captured)];
    }

    if (m.flag() == FLAG_PROMOTION) return 90000 + m.promoPiece();

    if (m == killers[0]) return 80000;
    if (m == killers[1]) return 79000;

    if (counterMove.data && m == counterMove) return 78000;

    return history[m.from()][m.to()];
}

static void sortMoves(const Position& pos, MoveList& moves, int* scores,
                      Move ttMove, const Move killers[2], Move counterMove,
                      const int history[64][64]) {
    for (int i = 0; i < moves.count; ++i) {
        scores[i] = scoreMove(pos, moves[i], ttMove, killers, counterMove, history);
    }
    // Insertion sort
    for (int i = 1; i < moves.count; ++i) {
        Move m = moves[i];
        int s = scores[i];
        int j = i - 1;
        while (j >= 0 && scores[j] < s) {
            moves[j + 1] = moves[j];
            scores[j + 1] = scores[j];
            --j;
        }
        moves[j + 1] = m;
        scores[j + 1] = s;
    }
}

static int scoreCaptureMove(const Position& pos, Move m) {
    Piece captured = pos.pieceOn(m.to());
    if (m.flag() == FLAG_ENPASSANT) captured = makePiece(~pos.sideToMove(), PAWN);
    if (captured == NO_PIECE) return 0;
    Piece mover = pos.pieceOn(m.from());
    return MVV_LVA[pieceType(mover)][pieceType(captured)];
}

static void sortCaptures(const Position& pos, MoveList& moves) {
    int scores[MAX_MOVES];
    for (int i = 0; i < moves.count; ++i)
        scores[i] = scoreCaptureMove(pos, moves[i]);
    for (int i = 1; i < moves.count; ++i) {
        Move m = moves[i];
        int s = scores[i];
        int j = i - 1;
        while (j >= 0 && scores[j] < s) {
            moves[j + 1] = moves[j];
            scores[j + 1] = scores[j];
            --j;
        }
        moves[j + 1] = m;
        scores[j + 1] = s;
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
//
// lmrTable[depth][moveNumber] -> base reduction. Built via a log formula that's
// standard in modern engines: gentle at low depth/count, grows with both.
static int lmrTable[MAX_PLY + 1][MAX_MOVES];

void Search::init() {
    for (int d = 0; d <= MAX_PLY; ++d) {
        for (int m = 0; m < MAX_MOVES; ++m) {
            if (d == 0 || m == 0) {
                lmrTable[d][m] = 0;
            } else {
                double r = 0.75 + std::log((double)d) * std::log((double)m) / 2.25;
                int ri = (int)r;
                if (ri < 0) ri = 0;
                lmrTable[d][m] = ri;
            }
        }
    }
}

// ---------- Time control ----------

void Search::checkTime() {
    if ((info.nodes & 2047) != 0) return;
    if (info.hardLimit <= 0) return;

    auto now = std::chrono::steady_clock::now();
    int elapsed = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - info.startTime).count();
    if (elapsed >= info.hardLimit) {
        stopped.store(true);
    }
}

void Search::allocateTime(const SearchLimits& limits, Color side) {
    info.softLimit = 0;
    info.hardLimit = 0;

    if (limits.movetime > 0) {
        info.softLimit = limits.movetime;
        info.hardLimit = limits.movetime;
        return;
    }
    if (limits.infinite) return;

    int timeLeft = (side == WHITE) ? limits.wtime : limits.btime;
    int inc = (side == WHITE) ? limits.winc : limits.binc;

    if (timeLeft <= 0) return;

    int movestogo = limits.movestogo > 0 ? limits.movestogo : 30;
    int soft = timeLeft / movestogo + inc * 3 / 4;
    if (soft < 10) soft = 10;
    if (soft > timeLeft / 2) soft = timeLeft / 2;

    // Hard limit gives the current iteration room to finish. Cap at timeLeft/3
    // so we never burn our entire budget on one move.
    int hard = soft * 4;
    int cap = timeLeft / 3;
    if (cap < 10) cap = 10;
    if (hard > cap) hard = cap;
    if (hard < soft) hard = soft;

    info.softLimit = soft;
    info.hardLimit = hard;
}

// ---------- Quiescence ----------

int Search::quiescence(Position& pos, int alpha, int beta, int ply) {
    if (stopped.load()) return 0;

    info.nodes++;

    int standPat = Eval::evaluate(pos);
    if (standPat >= beta) return beta;
    if (alpha < standPat) alpha = standPat;

    // Delta pruning
    const int DELTA = 1000; // queen value roughly
    if (standPat + DELTA < alpha) return alpha;

    MoveList captures;
    MoveGen::generateCaptures(pos, captures);
    sortCaptures(pos, captures);

    Color us = pos.sideToMove();

    for (int i = 0; i < captures.count; ++i) {
        StateInfo st;
        pos.makeMove(captures[i], st);
        if (pos.isSquareAttacked(pos.kingSq(us), pos.sideToMove())) {
            pos.unmakeMove(captures[i]);
            continue;
        }
        int score = -quiescence(pos, -beta, -alpha, ply + 1);
        pos.unmakeMove(captures[i]);

        if (stopped.load()) return 0;
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }

    return alpha;
}

// ---------- Main negamax ----------

int Search::negamax(Position& pos, int alpha, int beta, int depth, int ply, bool doNull) {
    if (stopped.load()) return 0;

    // Draw detection
    if (ply > 0 && pos.isDraw(ply)) return SCORE_DRAW;

    // Mate distance pruning: if we already know a faster mate, no point searching.
    if (ply > 0) {
        int mated   = -SCORE_MATE + ply;
        int mating  =  SCORE_MATE - ply - 1;
        if (alpha < mated)  alpha = mated;
        if (beta  > mating) beta  = mating;
        if (alpha >= beta) return alpha;
    }

    bool inCheck = pos.inCheck();

    // Check extension
    if (inCheck) depth++;

    if (depth <= 0) return quiescence(pos, alpha, beta, ply);

    info.nodes++;
    checkTime();

    bool pvNode = (beta - alpha > 1);

    // ---------- TT probe ----------
    bool ttHit = false;
    TTEntry* ttEntry = tt.probe(pos.key(), ttHit);
    Move ttMove = MOVE_NONE;
    int ttStaticEval = SCORE_NONE;
    if (ttHit) {
        ttMove = ttEntry->bestMove;
        ttStaticEval = ttEntry->staticEval;
        if (!pvNode && ttEntry->depth >= depth) {
            int ttScore = scoreFromTT(ttEntry->score, ply);
            if (ttEntry->flag == TT_EXACT) return ttScore;
            if (ttEntry->flag == TT_LOWER && ttScore >= beta) return ttScore;
            if (ttEntry->flag == TT_UPPER && ttScore <= alpha) return ttScore;
        }
    }

    // Use stored static eval if available; otherwise compute and stash a placeholder.
    int staticEval;
    if (inCheck) {
        staticEval = SCORE_NONE;
    } else if (ttHit && ttStaticEval != SCORE_NONE) {
        staticEval = ttStaticEval;
    } else {
        staticEval = Eval::evaluate(pos);
    }

    // Only run eval-dependent pruning when we have a valid static eval.
    bool prunable = !pvNode && !inCheck && staticEval != SCORE_NONE;

    // ---------- Reverse futility / static null move ----------
    if (prunable && depth <= 6) {
        int margin = 80 * depth;
        if (staticEval - margin >= beta) {
            return staticEval;
        }
    }

    // ---------- Razoring ----------
    if (prunable && depth <= 2) {
        if (staticEval + 300 < alpha) {
            int razorScore = quiescence(pos, alpha - 1, alpha, ply);
            if (razorScore < alpha) return razorScore;
        }
    }

    // ---------- Null move pruning ----------
    if (doNull && !inCheck && !pvNode && depth >= 3
        && staticEval != SCORE_NONE && staticEval >= beta) {
        Bitboard nonPawnMaterial = pos.pieces(pos.sideToMove()) &
            ~pos.pieces(pos.sideToMove(), PAWN) & ~pos.pieces(pos.sideToMove(), KING);
        if (nonPawnMaterial) {
            int R = 2 + depth / 4;
            StateInfo st;
            pos.doNullMove(st);
            int nullScore = -negamax(pos, -beta, -beta + 1, depth - R - 1, ply + 1, false);
            pos.undoNullMove();

            if (stopped.load()) return 0;
            if (nullScore >= beta) return beta;
        }
    }

    // ---------- Move generation and ordering ----------
    MoveList moves;
    MoveGen::generateLegal(pos, moves);

    if (moves.count == 0) {
        return inCheck ? (-SCORE_MATE + ply) : SCORE_DRAW;
    }

    // Counter-move lookup: previous move's [from][to] -> expected reply.
    Move prevMove = (ply > 0) ? info.prevMoveStack[ply - 1] : MOVE_NONE;
    Color us = pos.sideToMove();
    Move counterMove = MOVE_NONE;
    if (prevMove.data) {
        counterMove = info.counter[us][prevMove.from()][prevMove.to()];
    }

    int scores[MAX_MOVES];
    sortMoves(pos, moves, scores, ttMove, info.killers[ply], counterMove,
              info.history[us]);

    // Track quiet moves we tried so we can malus them on a cutoff.
    Move quietsTried[MAX_MOVES];
    int quietCount = 0;

    Move bestMove = MOVE_NONE;
    int bestScore = -SCORE_INF;
    TTFlag ttFlag = TT_UPPER;

    for (int i = 0; i < moves.count; ++i) {
        Move move = moves[i];

        Piece capturedBefore = pos.pieceOn(move.to());
        bool isCapture   = (capturedBefore != NO_PIECE) || (move.flag() == FLAG_ENPASSANT);
        bool isPromotion = (move.flag() == FLAG_PROMOTION);
        bool isKiller    = (move == info.killers[ply][0] || move == info.killers[ply][1]);
        bool isQuiet     = !isCapture && !isPromotion;

        // ---------- Shallow-depth forward pruning on quiet moves ----------
        // Skip late quiet moves that statically can't catch alpha.
        if (isQuiet && !pvNode && !inCheck && bestScore > -SCORE_MATE_IN_MAX_PLY) {
            // Late Move Pruning: drop quiet moves after a depth-dependent count.
            if (depth <= 4 && i >= 3 + depth * depth) {
                continue;
            }
            // Futility pruning at the frontier.
            if (depth <= 3 && staticEval != SCORE_NONE
                && staticEval + 90 + 80 * depth <= alpha) {
                continue;
            }
        }

        StateInfo st;
        pos.makeMove(move, st);

        bool givesCheck = pos.inCheck();

        // Record this move as the "previous move" for the child.
        info.prevMoveStack[ply] = move;

        int score;
        int newDepth = depth - 1;

        if (i == 0) {
            score = -negamax(pos, -beta, -alpha, newDepth, ply + 1, true);
        } else {
            // ---------- Late Move Reductions ----------
            int R = 0;
            bool lmrOk =
                depth >= 3 &&
                !inCheck &&
                !givesCheck &&
                !isCapture &&
                !isPromotion &&
                !isKiller &&
                i >= (pvNode ? 4 : 2);

            if (lmrOk) {
                R = lmrTable[std::min(depth, MAX_PLY)][std::min(i, MAX_MOVES - 1)];
                if (pvNode && R > 0) R -= 1;
                if (R > newDepth - 1) R = newDepth - 1; // keep reduced depth >= 1
                if (R < 0) R = 0;
            }

            score = -negamax(pos, -alpha - 1, -alpha, newDepth - R, ply + 1, true);

            if (!stopped.load() && R > 0 && score > alpha) {
                score = -negamax(pos, -alpha - 1, -alpha, newDepth, ply + 1, true);
            }

            if (!stopped.load() && pvNode && score > alpha && score < beta) {
                score = -negamax(pos, -beta, -alpha, newDepth, ply + 1, true);
            }
        }

        pos.unmakeMove(move);

        if (stopped.load()) return 0;

        // Track quiet moves so we can malus them if another quiet move cuts.
        if (isQuiet && quietCount < MAX_MOVES) {
            quietsTried[quietCount++] = move;
        }

        if (score > bestScore) {
            bestScore = score;
            bestMove = move;

            if (score > alpha) {
                alpha = score;
                ttFlag = TT_EXACT;

                if (score >= beta) {
                    ttFlag = TT_LOWER;

                    if (isQuiet) {
                        // Killers
                        if (info.killers[ply][0] != move) {
                            info.killers[ply][1] = info.killers[ply][0];
                            info.killers[ply][0] = move;
                        }
                        // History: bonus the cutoff move, malus the prior quiet moves.
                        int bonus = historyBonus(depth);
                        updateHistory(info.history[us][move.from()][move.to()], bonus);
                        for (int q = 0; q < quietCount - 1; ++q) {
                            Move bad = quietsTried[q];
                            updateHistory(info.history[us][bad.from()][bad.to()], -bonus);
                        }
                        // Counter-move
                        if (prevMove.data) {
                            info.counter[us][prevMove.from()][prevMove.to()] = move;
                        }
                    }
                    break;
                }
            }
        }
    }

    tt.store(pos.key(), scoreToTT(bestScore, ply), ttFlag, depth, bestMove,
             staticEval == SCORE_NONE ? 0 : staticEval);

    return bestScore;
}

// ---------- Root ----------

void Search::go(Position& pos, const SearchLimits& limits) {
    stopped.store(false);
    info = SearchInfo{};
    info.startTime = std::chrono::steady_clock::now();
    allocateTime(limits, pos.sideToMove());

    Move bestMove = MOVE_NONE;
    Move prevBest = MOVE_NONE;
    int bestScore = 0;
    int stableIters = 0;

    for (int depth = 1; depth <= limits.depth; ++depth) {
        int alpha = -SCORE_INF;
        int beta  =  SCORE_INF;

        if (depth >= 5) {
            alpha = bestScore - 30;
            beta  = bestScore + 30;
        }

        int score = negamax(pos, alpha, beta, depth, 0, true);

        if (!stopped.load() && (score <= alpha || score >= beta)) {
            score = negamax(pos, -SCORE_INF, SCORE_INF, depth, 0, true);
        }

        if (stopped.load()) break;

        bestScore = score;
        bestMove = info.bestMove = MOVE_NONE;

        bool ttHit;
        TTEntry* entry = tt.probe(pos.key(), ttHit);
        if (ttHit && entry->bestMove.data) {
            bestMove = entry->bestMove;
            info.bestMove = bestMove;
        }

        auto now = std::chrono::steady_clock::now();
        int elapsed = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - info.startTime).count();
        int64_t nps = elapsed > 0 ? (info.nodes * 1000) / elapsed : info.nodes;

        std::cout << "info depth " << depth
                  << " score ";
        if (bestScore > SCORE_MATE_IN_MAX_PLY)
            std::cout << "mate " << (SCORE_MATE - bestScore + 1) / 2;
        else if (bestScore < -SCORE_MATE_IN_MAX_PLY)
            std::cout << "mate -" << (SCORE_MATE + bestScore + 1) / 2;
        else
            std::cout << "cp " << bestScore;
        std::cout << " nodes " << info.nodes
                  << " nps " << nps
                  << " time " << elapsed
                  << std::endl;

        // Stability: if the best move isn't changing across iterations we can
        // stop slightly earlier; if it just changed, give ourselves more time.
        if (bestMove == prevBest) stableIters++;
        else                      stableIters = 0;
        prevBest = bestMove;

        // Stop if we've used our soft budget. Scale down slightly when the
        // best move has been stable for several iterations.
        if (info.softLimit > 0) {
            int scaledSoft = info.softLimit;
            if (stableIters >= 3) scaledSoft = (scaledSoft * 80) / 100;
            if (elapsed >= scaledSoft) break;
        }
    }

    if (!bestMove) {
        MoveList moves;
        MoveGen::generateLegal(pos, moves);
        if (moves.count > 0) bestMove = moves[0];
    }

    std::cout << "bestmove " << bestMove.toUCI() << std::endl;
}
