#pragma once

#include "types.h"
#include "position.h"

struct MoveList {
    Move moves[MAX_MOVES];
    int count = 0;

    void add(Move m) { moves[count++] = m; }
    Move& operator[](int i) { return moves[i]; }
    const Move& operator[](int i) const { return moves[i]; }
};

namespace MoveGen {

// Generate all pseudo-legal moves
void generatePseudoLegal(const Position& pos, MoveList& list);

// Generate all legal moves
void generateLegal(Position& pos, MoveList& list);

// Generate pseudo-legal captures + queen promotions (for qsearch)
void generateCaptures(const Position& pos, MoveList& list);

} // namespace MoveGen
