#pragma once

#include "position.h"
#include "types.h"

namespace SEE {

// Centipawn values for SEE swap calculations (independent from eval/PSTs).
// PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING, NO_PIECE_TYPE.
inline constexpr int PieceValue[7] = { 100, 325, 325, 500, 1000, 10000, 0 };

// Returns true iff the static-exchange-evaluation of `m` is >= `threshold`.
// Uses Stockfish-style iterative swap algorithm with x-ray re-resolution.
// Side-effect-free; does not call makeMove.
bool seeGE(const Position& pos, Move m, int threshold);

} // namespace SEE
