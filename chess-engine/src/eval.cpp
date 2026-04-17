#include "eval.h"
#include "bitboard.h"
#include <algorithm>

namespace Eval {

// PeSTO piece-square tables by Ronald Friederich
// Indexed by square (A1=0..H8=63), from white's perspective
// Middlegame tables
static const int mgPawnTable[64] = {
      0,   0,   0,   0,   0,   0,  0,   0,
     98, 134,  61,  95,  68, 126, 34, -11,
     -6,   7,  26,  31,  65,  56, 25, -20,
    -14,  13,   6,  21,  23,  12, 17, -23,
    -27,  -2,  -5,  12,  17,   6, 10, -25,
    -26,  -4,  -4, -10,   3,   3, 33, -12,
    -35,  -1, -20, -23, -15,  24, 38, -22,
      0,   0,   0,   0,   0,   0,  0,   0,
};

static const int mgKnightTable[64] = {
    -167, -89, -34, -49,  61, -97, -15, -107,
     -73, -41,  72,  36,  23,  62,   7,  -17,
     -47,  60,  37,  65,  84, 129,  73,   44,
      -9,  17,  19,  53,  37,  69,  18,   22,
     -13,   4,  16,  13,  28,  19,  21,   -8,
     -23,  -9,  12,  10,  19,  17,  25,  -16,
     -29, -53, -12,  -3,  -1,  18, -14,  -19,
    -105, -21, -58, -33, -17, -28, -19,  -23,
};

static const int mgBishopTable[64] = {
    -29,   4, -82, -37, -25, -42,   7,  -8,
    -26,  16, -18, -13,  30,  59,  18, -47,
    -16,  37,  43,  40,  35,  50,  37,  -2,
     -4,   5,  19,  50,  37,  37,   7,  -2,
     -6,  13,  13,  26,  34,  12,  10,   4,
      0,  15,  15,  15,  14,  27,  18,  10,
      4,  15,  16,   0,   7,  21,  33,   1,
    -33,  -3, -14, -21, -13, -12, -39, -21,
};

static const int mgRookTable[64] = {
     32,  42,  32,  51, 63,  9,  31,  43,
     27,  32,  58,  62, 80, 67,  26,  44,
     -5,  19,  26,  36, 17, 45,  61,  16,
    -24, -11,   7,  26, 24, 35,  -8, -20,
    -36, -26, -12,  -1,  9, -7,   6, -23,
    -45, -25, -16, -17,  3,  0,  -5, -33,
    -44, -16, -20,  -9, -1, 11,  -6, -71,
    -19, -13,   1,  17, 16,  7, -37, -26,
};

static const int mgQueenTable[64] = {
    -28,   0,  29,  12,  59,  44,  43,  45,
    -24, -39,  -5,   1, -16,  57,  28,  54,
    -13, -17,   7,   8,  29,  56,  47,  57,
    -27, -27, -16, -16,  -1,  17,  -2,   1,
     -9, -26,  -9, -10,  -2,  -4,   3,  -3,
    -14,   2, -11,  -2,  -5,   2,  14,   5,
    -35,  -8,  11,   2,   8,  15,  -3,   1,
     -1, -18,  -9,  10, -15, -25, -31, -50,
};

static const int mgKingTable[64] = {
    -65,  23,  16, -15, -56, -34,   2,  13,
     29,  -1, -20,  -7,  -8,  -4, -38, -29,
     -9,  24,   2, -16, -20,   6,  22, -22,
    -17, -20, -12, -27, -30, -25, -14, -36,
    -49,  -1, -27, -39, -46, -44, -33, -51,
    -14, -14, -22, -46, -44, -30, -15, -27,
      1,   7,  -8, -64, -43, -16,   9,   8,
    -15,  36,  12, -54,   8, -28,  24,  14,
};

// Endgame tables
static const int egPawnTable[64] = {
      0,   0,   0,   0,   0,   0,   0,   0,
    178, 173, 158, 134, 147, 132, 165, 187,
     94, 100,  85,  67,  56,  53,  82,  84,
     32,  24,  13,   5,  -2,   4,  17,  17,
     13,   9,  -3,  -7,  -7,  -8,   3,  -1,
      4,   7,  -6,   1,   0,  -5,  -1,  -8,
     13,   8,   8, -10,  -6,  -1,  -2,   6,
      0,   0,   0,   0,   0,   0,   0,   0,
};

static const int egKnightTable[64] = {
    -58, -38, -13, -28, -31, -27, -63, -99,
    -25,  -8, -25,  -2,  -9, -25, -24, -52,
    -24, -20,  10,   9,  -1,  -9, -19, -41,
    -17,   3,  22,  22,  22,  11,   8, -18,
    -18,  -6,  16,  25,  16,  17,   4, -18,
    -23,  -3,  -1,  15,  10,  -3, -20, -22,
    -42, -20, -10,  -5,  -2, -20, -23, -44,
    -29, -51, -23, -15, -22, -18, -50, -64,
};

static const int egBishopTable[64] = {
    -14, -21, -11,  -8, -7,  -9, -17, -24,
     -8,  -4,   7, -12, -3, -13,  -4, -14,
      2,  -8,   0,  -1, -2,   6,   0,   4,
     -3,   9,  12,   9, 14,  10,   3,   2,
     -6,   3,  13,  19,  7,  10,  -3,  -9,
    -12,  -3,   8,  10, 13,   3,  -7, -15,
    -14, -18,  -7,  -1,  4,  -9, -15, -27,
    -23,  -9, -23,  -5, -9, -16,  -5, -17,
};

static const int egRookTable[64] = {
     13, 10, 18, 15, 12,  12,   8,   5,
     11, 13, 13, 11, -3,   3,   8,   3,
      7,  7,  7,  5,  4,  -3,  -5,  -3,
      4,  3, 13,  1,  2,   1,  -1,   2,
      3,  5,  8,  4, -5,  -6,  -8, -11,
     -4,  0, -5, -1, -7, -12,  -8, -16,
     -6, -6,  0,  2, -9,  -9, -11,  -3,
     -9,  2,  3, -1, -5, -13,   4, -20,
};

static const int egQueenTable[64] = {
     -9,  22,  22,  27,  27,  19,  10,  20,
    -17,  20,  32,  41,  58,  25,  30,   0,
    -20,   6,   9,  49,  47,  35,  19,   9,
      3,  22,  24,  45,  57,  40,  57,  36,
    -18,  28,  19,  47,  31,  34,  39,  23,
    -16, -27,  15,   6,   9,  17,  10,   5,
    -22, -23, -30, -16, -16, -23, -36, -32,
    -33, -28, -22, -43,  -5, -32, -20, -41,
};

static const int egKingTable[64] = {
    -74, -35, -18, -18, -11,  15,   4, -17,
    -12,  17,  14,  17,  17,  38,  23,  11,
     10,  17,  23,  15,  20,  45,  44,  13,
     -8,  22,  24,  27,  26,  33,  26,   3,
    -18,  -4,  21,  24,  27,  23,   9, -11,
    -19,  -3,  11,  21,  23,  16,   7,  -9,
    -27, -11,   4,  13,  14,   4,  -5, -17,
    -53, -34, -21, -11, -28, -14, -24, -43,
};

// Piece base values for mg/eg
static const int mgPieceValue[6] = { 82, 337, 365, 477, 1025, 0 };
static const int egPieceValue[6] = { 94, 281, 297, 512,  936, 0 };

// Phase values per piece type
static const int phaseVal[6] = { 0, 1, 1, 2, 4, 0 };
static const int totalPhase = 24;

// Access PST: tables are stored with A1=index0, ranked from rank1 to rank8
static const int* mgTables[6] = {
    mgPawnTable, mgKnightTable, mgBishopTable,
    mgRookTable, mgQueenTable,  mgKingTable
};
static const int* egTables[6] = {
    egPawnTable, egKnightTable, egBishopTable,
    egRookTable, egQueenTable,  egKingTable
};

// --- v3 additions: structural / tactical eval terms ---

// Passed pawn bonus by relative rank (rank from pawn's side perspective).
static const int passedBonusMg[8] = { 0,  5, 10, 20, 35, 60,  90, 0 };
static const int passedBonusEg[8] = { 0, 15, 20, 35, 60, 95, 140, 0 };

// Bishop pair bonus
static const int bishopPairMg = 30;
static const int bishopPairEg = 50;

// Doubled / isolated pawn penalties (per affected pawn)
static const int doubledPawnMg = -10;
static const int doubledPawnEg = -20;
static const int isolatedPawnMg = -12;
static const int isolatedPawnEg = -18;

// Rook on open / semi-open file
static const int rookOpenMg = 25;
static const int rookOpenEg = 10;
static const int rookSemiOpenMg = 15;
static const int rookSemiOpenEg = 5;

// Mobility scalars (per attacked square in the "safe" area)
// Tuned conservatively - these only need to be directionally right.
static const int mobilityMg[6] = { 0, 4, 4, 2, 1, 0 };
static const int mobilityEg[6] = { 0, 4, 4, 4, 2, 0 };

// King safety attack weights per attacker piece type
static const int attackerWeight[6] = { 0, 2, 2, 3, 5, 0 };

// Pawn shield: bonus per friendly pawn in front of a castled king (first two ranks)
static const int pawnShieldBonus = 12;

// Precomputed tables initialized by Eval::init()
// PassedPawnMask[color][sq] = squares where an enemy pawn would block this pawn from being passed.
static Bitboard PassedPawnMask[2][64];
// IsolatedMask[file] = bitboard of adjacent files.
static Bitboard IsolatedMask[8];
// KingZone[color][sq] = king square + neighbours + one rank further forward.
static Bitboard KingZone[2][64];
// Forward file(s) used for pawn-shield detection.
static Bitboard PawnShieldMask[2][64];

void init() {
    for (int sq = 0; sq < 64; ++sq) {
        int f = fileOf(Square(sq));
        int r = rankOf(Square(sq));

        Bitboard adj = 0;
        if (f > 0) adj |= fileBB(f - 1);
        if (f < 7) adj |= fileBB(f + 1);
        Bitboard fileAndAdj = fileBB(f) | adj;

        Bitboard whiteFront = 0, blackFront = 0;
        for (int rr = r + 1; rr < 8; ++rr) whiteFront |= rankBB(rr);
        for (int rr = r - 1; rr >= 0; --rr) blackFront |= rankBB(rr);

        PassedPawnMask[WHITE][sq] = fileAndAdj & whiteFront;
        PassedPawnMask[BLACK][sq] = fileAndAdj & blackFront;

        // King zone: king neighbours plus a one-rank forward extension.
        Bitboard baseZone = BB::KingAttacks[sq] | squareBB(Square(sq));
        Bitboard forwardW = (r < 7) ? (BB::KingAttacks[sq] & rankBB(r + 1)) : 0;
        Bitboard forwardB = (r > 0) ? (BB::KingAttacks[sq] & rankBB(r - 1)) : 0;
        KingZone[WHITE][sq] = baseZone | forwardW;
        KingZone[BLACK][sq] = baseZone | forwardB;

        // Pawn shield: three files around king, two ranks in front of king.
        Bitboard shieldFiles = fileBB(f);
        if (f > 0) shieldFiles |= fileBB(f - 1);
        if (f < 7) shieldFiles |= fileBB(f + 1);

        Bitboard wShield = 0, bShield = 0;
        if (r + 1 < 8) wShield |= rankBB(r + 1);
        if (r + 2 < 8) wShield |= rankBB(r + 2);
        if (r - 1 >= 0) bShield |= rankBB(r - 1);
        if (r - 2 >= 0) bShield |= rankBB(r - 2);
        PawnShieldMask[WHITE][sq] = shieldFiles & wShield;
        PawnShieldMask[BLACK][sq] = shieldFiles & bShield;
    }

    for (int f = 0; f < 8; ++f) {
        Bitboard m = 0;
        if (f > 0) m |= fileBB(f - 1);
        if (f < 7) m |= fileBB(f + 1);
        IsolatedMask[f] = m;
    }
}

// Compute squares attacked by pawns of a given color.
static inline Bitboard pawnAttacksBB(Color c, Bitboard pawns) {
    if (c == WHITE) {
        Bitboard l = (pawns & ~FileABB) << 7;
        Bitboard r = (pawns & ~FileHBB) << 9;
        return l | r;
    } else {
        Bitboard l = (pawns & ~FileABB) >> 9;
        Bitboard r = (pawns & ~FileHBB) >> 7;
        return l | r;
    }
}

// Evaluate all non-PST terms for one side; accumulate into mg/eg (positive = good for `us`).
// Also feeds king-safety attack units via out parameters.
static void evaluateSide(const Position& pos, Color us,
                         int& mg, int& eg,
                         int& attackUnits, int& attackers,
                         Bitboard pawnAtkUs, Bitboard pawnAtkThem,
                         Bitboard occAll) {
    (void)pawnAtkUs; // not used on `us` side currently; kept for symmetry / future

    Color them = ~us;
    Square ksqThem = pos.kingSq(them);
    Bitboard kingZone = KingZone[them][ksqThem];

    // ---------- Pawns: passed / doubled / isolated ----------
    Bitboard ourPawns   = pos.pieces(us, PAWN);
    Bitboard theirPawns = pos.pieces(them, PAWN);

    // Doubled: any pawn on a file with >= 2 friendly pawns contributes a penalty
    // (count extras so N-stacked file costs (N-1) times).
    for (int f = 0; f < 8; ++f) {
        int cnt = BB::popcount(ourPawns & fileBB(f));
        if (cnt > 1) {
            mg += (cnt - 1) * doubledPawnMg;
            eg += (cnt - 1) * doubledPawnEg;
        }
    }

    Bitboard bb = ourPawns;
    while (bb) {
        Square s = BB::poplsb(bb);
        int f = fileOf(s);
        int r = rankOf(s);
        int relRank = (us == WHITE) ? r : (7 - r);

        // Passed?
        if ((PassedPawnMask[us][s] & theirPawns) == 0) {
            mg += passedBonusMg[relRank];
            eg += passedBonusEg[relRank];
        }

        // Isolated? no friendly pawn on adjacent files at all
        if ((IsolatedMask[f] & ourPawns) == 0) {
            mg += isolatedPawnMg;
            eg += isolatedPawnEg;
        }
    }

    // ---------- Bishop pair ----------
    if (BB::popcount(pos.pieces(us, BISHOP)) >= 2) {
        mg += bishopPairMg;
        eg += bishopPairEg;
    }

    // ---------- Rooks on (semi-)open files ----------
    Bitboard rooks = pos.pieces(us, ROOK);
    while (rooks) {
        Square s = BB::poplsb(rooks);
        Bitboard fileMask = fileBB(fileOf(s));
        bool ownPawnOnFile   = (fileMask & ourPawns) != 0;
        bool enemyPawnOnFile = (fileMask & theirPawns) != 0;
        if (!ownPawnOnFile) {
            if (!enemyPawnOnFile) {
                mg += rookOpenMg;
                eg += rookOpenEg;
            } else {
                mg += rookSemiOpenMg;
                eg += rookSemiOpenEg;
            }
        }
    }

    // ---------- Mobility + king-zone attacks ----------
    // "Safe" mobility mask: exclude own pieces and squares attacked by enemy pawns.
    Bitboard safe = ~pos.pieces(us) & ~pawnAtkThem;

    // Knights
    Bitboard knights = pos.pieces(us, KNIGHT);
    while (knights) {
        Square s = BB::poplsb(knights);
        Bitboard atk = BB::KnightAttacks[s];
        int m = BB::popcount(atk & safe);
        mg += m * mobilityMg[KNIGHT];
        eg += m * mobilityEg[KNIGHT];
        Bitboard kz = atk & kingZone;
        if (kz) { attackUnits += attackerWeight[KNIGHT] * BB::popcount(kz); attackers++; }
    }

    // Bishops
    Bitboard bishops = pos.pieces(us, BISHOP);
    while (bishops) {
        Square s = BB::poplsb(bishops);
        Bitboard atk = BB::bishopAttacks(s, occAll);
        int m = BB::popcount(atk & safe);
        mg += m * mobilityMg[BISHOP];
        eg += m * mobilityEg[BISHOP];
        Bitboard kz = atk & kingZone;
        if (kz) { attackUnits += attackerWeight[BISHOP] * BB::popcount(kz); attackers++; }
    }

    // Rooks
    Bitboard rks = pos.pieces(us, ROOK);
    while (rks) {
        Square s = BB::poplsb(rks);
        Bitboard atk = BB::rookAttacks(s, occAll);
        int m = BB::popcount(atk & safe);
        mg += m * mobilityMg[ROOK];
        eg += m * mobilityEg[ROOK];
        Bitboard kz = atk & kingZone;
        if (kz) { attackUnits += attackerWeight[ROOK] * BB::popcount(kz); attackers++; }
    }

    // Queens
    Bitboard queens = pos.pieces(us, QUEEN);
    while (queens) {
        Square s = BB::poplsb(queens);
        Bitboard atk = BB::queenAttacks(s, occAll);
        int m = BB::popcount(atk & safe);
        mg += m * mobilityMg[QUEEN];
        eg += m * mobilityEg[QUEEN];
        Bitboard kz = atk & kingZone;
        if (kz) { attackUnits += attackerWeight[QUEEN] * BB::popcount(kz); attackers++; }
    }
}

// Pawn-shield bonus for our king (MG only).
static inline int pawnShieldBonusFor(const Position& pos, Color us) {
    Square ks = pos.kingSq(us);
    Bitboard pawns = pos.pieces(us, PAWN);
    int shieldPawns = BB::popcount(PawnShieldMask[us][ks] & pawns);
    return shieldPawns * pawnShieldBonus;
}

int evaluate(const Position& pos) {
    int mgScore[2] = { 0, 0 };
    int egScore[2] = { 0, 0 };
    int phase = 0;

    // PST + material (unchanged from v2)
    for (int c = WHITE; c <= BLACK; ++c) {
        for (int pt = PAWN; pt <= KING; ++pt) {
            Bitboard bb = pos.pieces(Color(c), PieceType(pt));
            while (bb) {
                Square sq = BB::poplsb(bb);
                int idx = (c == WHITE) ? int(sq) : int(sq ^ 56);
                mgScore[c] += mgPieceValue[pt] + mgTables[pt][idx];
                egScore[c] += egPieceValue[pt] + egTables[pt][idx];
                phase += phaseVal[pt];
            }
        }
    }

    // Precompute pawn attacks & occupancy (used by both sides).
    Bitboard whitePawnAtk = pawnAttacksBB(WHITE, pos.pieces(WHITE, PAWN));
    Bitboard blackPawnAtk = pawnAttacksBB(BLACK, pos.pieces(BLACK, PAWN));
    Bitboard occAll = pos.allPieces();

    // Per-side structural + mobility + king-zone attacks.
    int atkUnits[2] = { 0, 0 };
    int attackers[2] = { 0, 0 };
    evaluateSide(pos, WHITE, mgScore[WHITE], egScore[WHITE],
                 atkUnits[WHITE], attackers[WHITE],
                 whitePawnAtk, blackPawnAtk, occAll);
    evaluateSide(pos, BLACK, mgScore[BLACK], egScore[BLACK],
                 atkUnits[BLACK], attackers[BLACK],
                 blackPawnAtk, whitePawnAtk, occAll);

    // Pawn shield bonus (MG)
    mgScore[WHITE] += pawnShieldBonusFor(pos, WHITE);
    mgScore[BLACK] += pawnShieldBonusFor(pos, BLACK);

    // King-safety penalty (MG only): quadratic in attackers present, capped.
    // Only apply when there are at least 2 attacking piece-types -
    // one piece stranding near the enemy king shouldn't crater eval.
    for (int c = WHITE; c <= BLACK; ++c) {
        if (attackers[c] >= 2) {
            int units = atkUnits[c];
            int penalty = (units * units) / 4;
            if (penalty > 500) penalty = 500;
            // This is a penalty for the *defender* (opposite color king).
            mgScore[~Color(c)] -= penalty;
        }
    }

    if (phase > totalPhase) phase = totalPhase;

    int mg = mgScore[WHITE] - mgScore[BLACK];
    int eg = egScore[WHITE] - egScore[BLACK];

    int score = (mg * phase + eg * (totalPhase - phase)) / totalPhase;

    // Tempo bonus
    score += 10;

    return (pos.sideToMove() == WHITE) ? score : -score;
}

} // namespace Eval
