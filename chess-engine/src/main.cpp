#include "bitboard.h"
#include "magics.h"
#include "zobrist.h"
#include "eval.h"
#include "search.h"
#include "uci.h"

// Bench signature: 5895645 nodes at depth 13. Source of truth: BENCH_SIGNATURE in src/uci.cpp.

int main() {
    BB::init();
    Magics::init();
    Zobrist::init();
    Eval::init();
    Search::init();

    UCI::loop();

    return 0;
}
