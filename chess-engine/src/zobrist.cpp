#include "zobrist.h"
#include <random>

namespace Zobrist {

uint64_t PieceSquare[16][64];
uint64_t Castling[16];
uint64_t EnPassant[8];
uint64_t SideToMove;

void init() {
    std::mt19937_64 rng(1070372); // fixed seed for reproducibility

    for (int p = 0; p < 16; ++p)
        for (int sq = 0; sq < 64; ++sq)
            PieceSquare[p][sq] = rng();

    for (int i = 0; i < 16; ++i)
        Castling[i] = rng();

    for (int f = 0; f < 8; ++f)
        EnPassant[f] = rng();

    SideToMove = rng();
}

} // namespace Zobrist
