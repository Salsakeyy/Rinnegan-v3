#include "bitboard.h"
#include "magics.h"
#include "zobrist.h"
#include "uci.h"

int main() {
    BB::init();
    Magics::init();
    Zobrist::init();

    UCI::loop();

    return 0;
}
