#pragma once

#include "types.h"

namespace BB {

void init();

// Bit manipulation
inline int popcount(Bitboard b) { return __builtin_popcountll(b); }
inline Square lsb(Bitboard b) { return Square(__builtin_ctzll(b)); }
inline Square poplsb(Bitboard& b) {
    Square s = lsb(b);
    b &= b - 1;
    return s;
}
inline bool moreThanOne(Bitboard b) { return b & (b - 1); }

// Precomputed attack tables
extern Bitboard PawnAttacks[2][64];
extern Bitboard KnightAttacks[64];
extern Bitboard KingAttacks[64];
extern Bitboard BetweenBB[64][64];
extern Bitboard LineBB[64][64];

// Sliding attacks (forwarded to magics module)
Bitboard rookAttacks(Square s, Bitboard occ);
Bitboard bishopAttacks(Square s, Bitboard occ);
Bitboard queenAttacks(Square s, Bitboard occ);

// Sliding attacks without occupancy (for masks)
Bitboard rookAttacksSlow(Square s, Bitboard occ);
Bitboard bishopAttacksSlow(Square s, Bitboard occ);

} // namespace BB
