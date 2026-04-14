#include "../src/bitboard.h"
#include "../src/magics.h"
#include "../src/zobrist.h"
#include "../src/position.h"
#include "../src/perft.h"
#include <iostream>
#include <string>

struct PerftTest {
    std::string name;
    std::string fen;
    int depth;
    uint64_t expected;
};

int main() {
    BB::init();
    Magics::init();
    Zobrist::init();

    PerftTest tests[] = {
        { "Startpos",   "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5, 4865609ULL },
        { "Kiwipete",   "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603ULL },
        { "Position 3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624ULL },
        { "Position 4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333ULL },
        { "Position 5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487ULL },
        { "Position 6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594ULL },
    };

    int passed = 0;
    int total = sizeof(tests) / sizeof(tests[0]);

    for (auto& test : tests) {
        Position pos;
        pos.setFromFen(test.fen);
        std::cout << "Testing " << test.name << " (depth " << test.depth << ")... " << std::flush;
        uint64_t result = Perft::perft(pos, test.depth);
        if (result == test.expected) {
            std::cout << "PASS (" << result << ")" << std::endl;
            passed++;
        } else {
            std::cout << "FAIL (got " << result << ", expected " << test.expected << ")" << std::endl;
        }
    }

    std::cout << "\n" << passed << "/" << total << " tests passed." << std::endl;
    return (passed == total) ? 0 : 1;
}
