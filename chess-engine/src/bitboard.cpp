#include "bitboard.h"
#include "magics.h"

namespace BB {

Bitboard PawnAttacks[2][64];
Bitboard KnightAttacks[64];
Bitboard KingAttacks[64];
Bitboard BetweenBB[64][64];
Bitboard LineBB[64][64];

// Slow ray-based sliding attack generation (used for magic init + between/line)
Bitboard rookAttacksSlow(Square s, Bitboard occ) {
    Bitboard attacks = 0;
    int r = rankOf(s), f = fileOf(s);
    // North
    for (int rr = r + 1; rr <= 7; ++rr) {
        Bitboard sq = 1ULL << (rr * 8 + f);
        attacks |= sq;
        if (occ & sq) break;
    }
    // South
    for (int rr = r - 1; rr >= 0; --rr) {
        Bitboard sq = 1ULL << (rr * 8 + f);
        attacks |= sq;
        if (occ & sq) break;
    }
    // East
    for (int ff = f + 1; ff <= 7; ++ff) {
        Bitboard sq = 1ULL << (r * 8 + ff);
        attacks |= sq;
        if (occ & sq) break;
    }
    // West
    for (int ff = f - 1; ff >= 0; --ff) {
        Bitboard sq = 1ULL << (r * 8 + ff);
        attacks |= sq;
        if (occ & sq) break;
    }
    return attacks;
}

Bitboard bishopAttacksSlow(Square s, Bitboard occ) {
    Bitboard attacks = 0;
    int r = rankOf(s), f = fileOf(s);
    // NE
    for (int rr = r + 1, ff = f + 1; rr <= 7 && ff <= 7; ++rr, ++ff) {
        Bitboard sq = 1ULL << (rr * 8 + ff);
        attacks |= sq;
        if (occ & sq) break;
    }
    // NW
    for (int rr = r + 1, ff = f - 1; rr <= 7 && ff >= 0; ++rr, --ff) {
        Bitboard sq = 1ULL << (rr * 8 + ff);
        attacks |= sq;
        if (occ & sq) break;
    }
    // SE
    for (int rr = r - 1, ff = f + 1; rr >= 0 && ff <= 7; --rr, ++ff) {
        Bitboard sq = 1ULL << (rr * 8 + ff);
        attacks |= sq;
        if (occ & sq) break;
    }
    // SW
    for (int rr = r - 1, ff = f - 1; rr >= 0 && ff >= 0; --rr, --ff) {
        Bitboard sq = 1ULL << (rr * 8 + ff);
        attacks |= sq;
        if (occ & sq) break;
    }
    return attacks;
}

static void initPawnAttacks() {
    for (int sq = 0; sq < 64; ++sq) {
        Bitboard b = squareBB(Square(sq));
        // White pawn attacks
        PawnAttacks[WHITE][sq] = ((b & ~FileABB) << 7) | ((b & ~FileHBB) << 9);
        // Black pawn attacks
        PawnAttacks[BLACK][sq] = ((b & ~FileHBB) >> 7) | ((b & ~FileABB) >> 9);
    }
}

static void initKnightAttacks() {
    for (int sq = 0; sq < 64; ++sq) {
        Bitboard b = squareBB(Square(sq));
        Bitboard attacks = 0;
        attacks |= (b & ~FileABB & ~FileBBB) << 6;   // NNW-like: left 2, up 1
        attacks |= (b & ~FileABB)            << 15;   // left 1, up 2
        attacks |= (b & ~FileHBB)            << 17;   // right 1, up 2
        attacks |= (b & ~FileGBB & ~FileHBB) << 10;   // right 2, up 1
        attacks |= (b & ~FileGBB & ~FileHBB) >> 6;    // right 2, down 1
        attacks |= (b & ~FileHBB)            >> 15;   // right 1, down 2
        attacks |= (b & ~FileABB)            >> 17;   // left 1, down 2
        attacks |= (b & ~FileABB & ~FileBBB) >> 10;   // left 2, down 1
        KnightAttacks[sq] = attacks;
    }
}

static void initKingAttacks() {
    for (int sq = 0; sq < 64; ++sq) {
        Bitboard b = squareBB(Square(sq));
        Bitboard attacks = 0;
        attacks |= (b & ~FileABB) << 7;
        attacks |= b << 8;
        attacks |= (b & ~FileHBB) << 9;
        attacks |= (b & ~FileHBB) << 1;
        attacks |= (b & ~FileHBB) >> 7;
        attacks |= b >> 8;
        attacks |= (b & ~FileABB) >> 9;
        attacks |= (b & ~FileABB) >> 1;
        KingAttacks[sq] = attacks;
    }
}

static void initBetweenAndLine() {
    for (int s1 = 0; s1 < 64; ++s1) {
        for (int s2 = 0; s2 < 64; ++s2) {
            BetweenBB[s1][s2] = 0;
            LineBB[s1][s2] = 0;
        }
    }

    for (int s1 = 0; s1 < 64; ++s1) {
        // Rook lines
        Bitboard rattk = rookAttacksSlow(Square(s1), 0);
        for (int s2 = 0; s2 < 64; ++s2) {
            if (s1 == s2) continue;
            if (rattk & squareBB(Square(s2))) {
                // They are on the same rank or file
                LineBB[s1][s2] = (rookAttacksSlow(Square(s1), 0) & rookAttacksSlow(Square(s2), 0))
                                 | squareBB(Square(s1)) | squareBB(Square(s2));
                BetweenBB[s1][s2] = rookAttacksSlow(Square(s1), squareBB(Square(s2)))
                                  & rookAttacksSlow(Square(s2), squareBB(Square(s1)));
            }
        }
        // Bishop lines
        Bitboard battk = bishopAttacksSlow(Square(s1), 0);
        for (int s2 = 0; s2 < 64; ++s2) {
            if (s1 == s2) continue;
            if (battk & squareBB(Square(s2))) {
                LineBB[s1][s2] = (bishopAttacksSlow(Square(s1), 0) & bishopAttacksSlow(Square(s2), 0))
                                 | squareBB(Square(s1)) | squareBB(Square(s2));
                BetweenBB[s1][s2] = bishopAttacksSlow(Square(s1), squareBB(Square(s2)))
                                  & bishopAttacksSlow(Square(s2), squareBB(Square(s1)));
            }
        }
    }
}

void init() {
    initPawnAttacks();
    initKnightAttacks();
    initKingAttacks();
    initBetweenAndLine();
}

Bitboard rookAttacks(Square s, Bitboard occ) { return Magics::rookAttacks(s, occ); }
Bitboard bishopAttacks(Square s, Bitboard occ) { return Magics::bishopAttacks(s, occ); }
Bitboard queenAttacks(Square s, Bitboard occ) { return Magics::rookAttacks(s, occ) | Magics::bishopAttacks(s, occ); }

} // namespace BB
