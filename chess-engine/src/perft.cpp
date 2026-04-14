#include "perft.h"
#include "movegen.h"
#include <iostream>

namespace Perft {

uint64_t perft(Position& pos, int depth) {
    if (depth == 0) return 1;

    MoveList moves;
    MoveGen::generateLegal(pos, moves);

    if (depth == 1) return moves.count;

    uint64_t nodes = 0;
    for (int i = 0; i < moves.count; ++i) {
        StateInfo st;
        pos.makeMove(moves[i], st);
        nodes += perft(pos, depth - 1);
        pos.unmakeMove(moves[i]);
    }
    return nodes;
}

void divide(Position& pos, int depth) {
    MoveList moves;
    MoveGen::generateLegal(pos, moves);

    uint64_t total = 0;
    for (int i = 0; i < moves.count; ++i) {
        StateInfo st;
        pos.makeMove(moves[i], st);
        uint64_t nodes = (depth <= 1) ? 1 : perft(pos, depth - 1);
        pos.unmakeMove(moves[i]);
        std::cout << moves[i].toUCI() << ": " << nodes << std::endl;
        total += nodes;
    }
    std::cout << "\nTotal: " << total << std::endl;
}

} // namespace Perft
