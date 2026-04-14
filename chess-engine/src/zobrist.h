#pragma once

#include "types.h"

namespace Zobrist {

void init();

extern uint64_t PieceSquare[16][64]; // indexed by Piece enum
extern uint64_t Castling[16];
extern uint64_t EnPassant[8]; // indexed by file
extern uint64_t SideToMove;

} // namespace Zobrist
