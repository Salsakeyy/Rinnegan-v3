#include "magics.h"
#include "bitboard.h"
#include <cstring>
#include <random>

namespace Magics {

struct MagicEntry {
    Bitboard mask;
    Bitboard magic;
    int shift;
    Bitboard* attacks;
};

static MagicEntry RookMagics[64];
static MagicEntry BishopMagics[64];

static Bitboard RookTable[64 * 4096];
static Bitboard BishopTable[64 * 512];

static Bitboard rookMask(Square s) {
    Bitboard mask = 0;
    int r = rankOf(s), f = fileOf(s);
    for (int rr = r + 1; rr <= 6; ++rr) mask |= 1ULL << (rr * 8 + f);
    for (int rr = r - 1; rr >= 1; --rr) mask |= 1ULL << (rr * 8 + f);
    for (int ff = f + 1; ff <= 6; ++ff) mask |= 1ULL << (r * 8 + ff);
    for (int ff = f - 1; ff >= 1; --ff) mask |= 1ULL << (r * 8 + ff);
    return mask;
}

static Bitboard bishopMask(Square s) {
    Bitboard mask = 0;
    int r = rankOf(s), f = fileOf(s);
    for (int rr = r + 1, ff = f + 1; rr <= 6 && ff <= 6; ++rr, ++ff) mask |= 1ULL << (rr * 8 + ff);
    for (int rr = r + 1, ff = f - 1; rr <= 6 && ff >= 1; ++rr, --ff) mask |= 1ULL << (rr * 8 + ff);
    for (int rr = r - 1, ff = f + 1; rr >= 1 && ff <= 6; --rr, ++ff) mask |= 1ULL << (rr * 8 + ff);
    for (int rr = r - 1, ff = f - 1; rr >= 1 && ff >= 1; --rr, --ff) mask |= 1ULL << (rr * 8 + ff);
    return mask;
}

static Bitboard sparseRandom(std::mt19937_64& rng) {
    return rng() & rng() & rng();
}

// Find a magic number for a given square by trial
static Bitboard findMagic(Square sq, bool isRook, Bitboard* table, int tableSize) {
    Bitboard mask = isRook ? rookMask(sq) : bishopMask(sq);
    int bits = BB::popcount(mask);
    int shift = 64 - bits;
    int numSubsets = 1 << bits;

    // Enumerate all subsets and their correct attack sets
    Bitboard occupancies[4096];
    Bitboard attacks[4096];
    Bitboard occ = 0;
    int idx = 0;
    do {
        occupancies[idx] = occ;
        attacks[idx] = isRook ? BB::rookAttacksSlow(sq, occ) : BB::bishopAttacksSlow(sq, occ);
        idx++;
        occ = (occ - mask) & mask;
    } while (occ);

    std::mt19937_64 rng(sq + (isRook ? 0 : 64) + 12345);

    // Try random magics
    for (int attempt = 0; attempt < 100000000; ++attempt) {
        Bitboard magic = sparseRandom(rng);

        // Quick reject: need enough bits in the upper part
        if (BB::popcount((mask * magic) & 0xFF00000000000000ULL) < 6) continue;

        // Clear table
        Bitboard used[4096];
        std::memset(used, 0, sizeof(Bitboard) * numSubsets);
        bool fail = false;

        for (int i = 0; i < numSubsets; ++i) {
            int index = (int)((occupancies[i] * magic) >> shift);
            if (used[index] == 0) {
                used[index] = attacks[i];
            } else if (used[index] != attacks[i]) {
                fail = true;
                break;
            }
        }

        if (!fail) {
            // Found a working magic — populate the actual table
            std::memset(table, 0, sizeof(Bitboard) * tableSize);
            for (int i = 0; i < numSubsets; ++i) {
                int index = (int)((occupancies[i] * magic) >> shift);
                table[index] = attacks[i];
            }
            return magic;
        }
    }

    // Should never happen
    return 0;
}

void init() {
    for (int sq = 0; sq < 64; ++sq) {
        Bitboard rmask = rookMask(Square(sq));
        int rbits = BB::popcount(rmask);
        RookMagics[sq].mask = rmask;
        RookMagics[sq].shift = 64 - rbits;
        RookMagics[sq].attacks = &RookTable[sq * 4096];
        RookMagics[sq].magic = findMagic(Square(sq), true, RookMagics[sq].attacks, 4096);

        Bitboard bmask = bishopMask(Square(sq));
        int bbits = BB::popcount(bmask);
        BishopMagics[sq].mask = bmask;
        BishopMagics[sq].shift = 64 - bbits;
        BishopMagics[sq].attacks = &BishopTable[sq * 512];
        BishopMagics[sq].magic = findMagic(Square(sq), false, BishopMagics[sq].attacks, 512);
    }
}

Bitboard rookAttacks(Square s, Bitboard occ) {
    const MagicEntry& e = RookMagics[s];
    return e.attacks[((occ & e.mask) * e.magic) >> e.shift];
}

Bitboard bishopAttacks(Square s, Bitboard occ) {
    const MagicEntry& e = BishopMagics[s];
    return e.attacks[((occ & e.mask) * e.magic) >> e.shift];
}

} // namespace Magics
