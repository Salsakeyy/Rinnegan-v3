#include "bitboard.h"
#include "magics.h"
#include "zobrist.h"
#include "eval.h"
#include "search.h"
#include "uci.h"

int main() {
    BB::init();
    Magics::init();
    Zobrist::init();
    Eval::init();
    Search::init();

    UCI::loop();

    return 0;
}
