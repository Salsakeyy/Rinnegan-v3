#include "search.h"
#include "eval.h"
#include "movegen.h"
#include <iostream>
#include <algorithm>

// MVV-LVA scoring for move ordering
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

static int scoreMove(const Position& pos, Move m, Move ttMove,
                     const Move killers[2], const int history[64][64]) {
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

    return history[m.from()][m.to()];
}

static void sortMoves(const Position& pos, MoveList& moves, Move ttMove,
                      const Move killers[2], const int history[64][64]) {
    int scores[MAX_MOVES];
    for (int i = 0; i < moves.count; ++i) {
        scores[i] = scoreMove(pos, moves[i], ttMove, killers, history);
    }
    // Insertion sort (small arrays)
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

// Mate score adjustment for TT
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

void Search::checkTime() {
    if ((info.nodes & 2047) != 0) return;
    if (info.allocatedTime <= 0) return;

    auto now = std::chrono::steady_clock::now();
    int elapsed = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - info.startTime).count();
    if (elapsed >= info.allocatedTime) {
        stopped.store(true);
    }
}

int Search::allocateTime(const SearchLimits& limits, Color side) {
    if (limits.movetime > 0) return limits.movetime;
    if (limits.infinite) return 0;

    int timeLeft = (side == WHITE) ? limits.wtime : limits.btime;
    int inc = (side == WHITE) ? limits.winc : limits.binc;

    if (timeLeft <= 0) return 0;

    int movestogo = limits.movestogo > 0 ? limits.movestogo : 30;
    int allocated = timeLeft / movestogo + inc * 3 / 4;

    // Don't use more than 50% of remaining time
    allocated = std::min(allocated, timeLeft / 2);

    return std::max(allocated, 10); // at least 10ms
}

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

int Search::negamax(Position& pos, int alpha, int beta, int depth, int ply, bool doNull) {
    if (stopped.load()) return 0;

    // Check for draw
    if (ply > 0 && pos.isDraw(ply)) return SCORE_DRAW;

    bool inCheck = pos.inCheck();

    // Check extension
    if (inCheck) depth++;

    if (depth <= 0) return quiescence(pos, alpha, beta, ply);

    info.nodes++;
    checkTime();

    bool pvNode = (beta - alpha > 1);

    // TT probe
    bool ttHit = false;
    TTEntry* ttEntry = tt.probe(pos.key(), ttHit);
    Move ttMove = MOVE_NONE;
    if (ttHit) {
        ttMove = ttEntry->bestMove;
        if (!pvNode && ttEntry->depth >= depth) {
            int ttScore = scoreFromTT(ttEntry->score, ply);
            if (ttEntry->flag == TT_EXACT) return ttScore;
            if (ttEntry->flag == TT_LOWER && ttScore >= beta) return ttScore;
            if (ttEntry->flag == TT_UPPER && ttScore <= alpha) return ttScore;
        }
    }

    int staticEval = Eval::evaluate(pos);

    // Null move pruning
    if (doNull && !inCheck && !pvNode && depth >= 3 && staticEval >= beta) {
        // Skip null move if we only have king + pawns
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

    MoveList moves;
    MoveGen::generateLegal(pos, moves);

    if (moves.count == 0) {
        return inCheck ? (-SCORE_MATE + ply) : SCORE_DRAW;
    }

    sortMoves(pos, moves, ttMove, info.killers[ply], info.history[pos.sideToMove()]);

    Move bestMove = MOVE_NONE;
    int bestScore = -SCORE_INF;
    TTFlag ttFlag = TT_UPPER;

    for (int i = 0; i < moves.count; ++i) {
        StateInfo st;
        pos.makeMove(moves[i], st);
        int score = -negamax(pos, -beta, -alpha, depth - 1, ply + 1, true);
        pos.unmakeMove(moves[i]);

        if (stopped.load()) return 0;

        if (score > bestScore) {
            bestScore = score;
            bestMove = moves[i];

            if (score > alpha) {
                alpha = score;
                ttFlag = TT_EXACT;

                if (score >= beta) {
                    ttFlag = TT_LOWER;

                    // Update killers and history for quiet moves
                    Piece captured = pos.pieceOn(moves[i].to());
                    if (captured == NO_PIECE && moves[i].flag() != FLAG_ENPASSANT) {
                        if (info.killers[ply][0] != moves[i]) {
                            info.killers[ply][1] = info.killers[ply][0];
                            info.killers[ply][0] = moves[i];
                        }
                        info.history[pos.sideToMove()][moves[i].from()][moves[i].to()] += depth * depth;
                    }
                    break;
                }
            }
        }
    }

    tt.store(pos.key(), scoreToTT(bestScore, ply), ttFlag, depth, bestMove, staticEval);

    return bestScore;
}

void Search::go(Position& pos, const SearchLimits& limits) {
    stopped.store(false);
    info = SearchInfo{};
    info.startTime = std::chrono::steady_clock::now();
    info.allocatedTime = allocateTime(limits, pos.sideToMove());

    Move bestMove = MOVE_NONE;
    int bestScore = 0;

    for (int depth = 1; depth <= limits.depth; ++depth) {
        int alpha = -SCORE_INF;
        int beta = SCORE_INF;

        // Aspiration windows after depth 5
        if (depth >= 5) {
            alpha = bestScore - 30;
            beta = bestScore + 30;
        }

        int score = negamax(pos, alpha, beta, depth, 0, true);

        // Re-search with full window on aspiration failure
        if (!stopped.load() && (score <= alpha || score >= beta)) {
            score = negamax(pos, -SCORE_INF, SCORE_INF, depth, 0, true);
        }

        if (stopped.load()) break;

        bestScore = score;
        bestMove = info.bestMove = MOVE_NONE;

        // Get best move from TT
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

        // Check time after iteration
        if (info.allocatedTime > 0) {
            if (elapsed >= info.allocatedTime * 60 / 100) break;
        }
    }

    // Fallback: if no TT move found, generate legal moves
    if (!bestMove) {
        MoveList moves;
        MoveGen::generateLegal(pos, moves);
        if (moves.count > 0) bestMove = moves[0];
    }

    std::cout << "bestmove " << bestMove.toUCI() << std::endl;
}
