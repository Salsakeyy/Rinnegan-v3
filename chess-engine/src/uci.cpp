#include "uci.h"
#include "eval.h"
#include "policy.h"
#include "movegen.h"
#include "perft.h"
#include "position.h"
#include "search.h"
#include "tt.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace UCI {

bool usePolicy = false;
bool policyTimeMod = false;
int threads = 1;
int policyScale = 160;
int policyTimeMargin = 150;
int policyTimeBoost = 20;
int moveOverhead = 25;
std::string policyFile = "data/policy/stockfish-200k/train/smoke-policy.bin";

// Policy v2 state. PolicyMode controls how the (v1 or v2) bonus is fused
// into the root move ordering. 0=off, 1=root_bonus, 2=quiet_residual.
int policyMode = 1;
bool usePolicyV2 = false;
std::string policyV2File = "data/policy/v2/policy.bin";
int policyV2Mode = 2;
int policyV2Scale = 160;
int policyV2QuietScale = 160;
int policyV2EndgameScale = 100;
int policyV2BonusClamp = 20000;

namespace {

// Golden bench node count at kBenchDepth. Re-pin after any patch that
// changes node visit order. CI greps `BENCH_SIGNATURE` to verify.
constexpr int64_t BENCH_SIGNATURE = 2782066;

constexpr const char* StartPosFen =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

static TranspositionTable tt;
static Search* searcher = nullptr;
static std::thread searchThread;

void stopSearch() {
    if (searcher) searcher->stop();
    if (searchThread.joinable()) searchThread.join();
}

Move parseMove(Position& pos, const std::string& str) {
    if (str.length() < 4) return MOVE_NONE;

    Square from = makeSquare(str[0] - 'a', str[1] - '1');
    Square to = makeSquare(str[2] - 'a', str[3] - '1');

    MoveList moves;
    MoveGen::generateLegal(pos, moves);

    for (int i = 0; i < moves.count; ++i) {
        Move move = moves[i];
        if (move.from() != from || move.to() != to) continue;

        if (move.flag() != FLAG_PROMOTION)
            return move;

        if (str.length() >= 5) {
            PieceType promo = QUEEN;
            switch (str[4]) {
            case 'n': promo = KNIGHT; break;
            case 'b': promo = BISHOP; break;
            case 'r': promo = ROOK;   break;
            case 'q': promo = QUEEN;  break;
            default: break;
            }
            if (move.promoPiece() == promo) return move;
        } else if (move.promoPiece() == QUEEN) {
            return move;
        }
    }

    return MOVE_NONE;
}

std::string trimLeft(std::string value) {
    size_t first = value.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return value.substr(first);
}

void parseSetOption(const std::string& line, std::string& name, std::string& value) {
    std::istringstream ss(line);
    std::string token;

    name.clear();
    value.clear();

    ss >> token; // setoption
    ss >> token; // name

    while (ss >> token) {
        if (token == "value") break;
        if (!name.empty()) name += ' ';
        name += token;
    }

    std::getline(ss, value);
    value = trimLeft(value);
}

bool isGoKeyword(const std::string& token) {
    return token == "searchmoves" ||
           token == "ponder" ||
           token == "wtime" ||
           token == "btime" ||
           token == "winc" ||
           token == "binc" ||
           token == "movestogo" ||
           token == "depth" ||
           token == "nodes" ||
           token == "mate" ||
           token == "movetime" ||
           token == "infinite";
}

void reportPolicyLoad(const std::string& path, bool loaded) {
    if (loaded)
        std::cout << "info string policy loaded from " << path << std::endl;
    else
        std::cout << "info string policy load failed" << std::endl;
}

void runBench() {
    static const char* kBenchFens[] = {
        StartPosFen,
        "r3k2r/p1ppqpb1/bn2pnp1/2pP4/1p2P3/2N2N2/PPQBBPPP/R3K2R w KQkq - 0 1",
        "4rrk1/2p1qppp/p1np1n2/1p2p3/4P3/1NN1BP2/PPP1Q1PP/2KR3R w - - 0 1",
        "r3q1k1/1pp2ppp/p1npbn2/4p3/4P3/1NN1BP2/PPP2QPP/2KR3R w - - 0 1",
        "2r2rk1/pp1n1ppp/2pbpn2/q7/3P4/2N1PN2/PPQ1BPPP/2RR2K1 w - - 0 1",
        "r1bq1rk1/pppn1ppp/3bpn2/3p4/3P4/2N1PN2/PPQ1BPPP/R1BR2K1 w - - 0 1",
        "8/2p5/3p4/kP1P4/3K4/8/8/8 w - - 0 1",
        "8/8/2k5/2Pp4/3K4/8/8/8 w - d6 0 1",
        "rnbq1rk1/pp3ppp/2p1pn2/3p4/3P4/2N1PN2/PPQ1BPPP/R1B1K2R w KQ - 0 1",
        "r1bq1rk1/ppp2ppp/2np1n2/4p3/2BPP3/2N2N2/PPP2PPP/R1BQ1RK1 b - - 0 1",
        "2r2rk1/1bqn1ppp/p3pn2/1p6/3P4/1BN1PN2/PPQ2PPP/2RR2K1 w - - 0 1",
        "r4rk1/1pp1qppp/p1np1n2/4p3/2BPP3/2N2N2/PPP2PPP/R2Q1RK1 w - - 0 1",
        "3r2k1/5ppp/1p2pn2/p1r5/P1P5/1P1R1NP1/5PBP/3R2K1 w - - 0 1",
        "6k1/5ppp/1p2pn2/p1r5/P1P5/1P1R1NP1/5PBP/3R2K1 b - - 0 1",
        "r1bqk2r/ppp2ppp/2np1n2/4p3/2BPP3/2N2N2/PPP2PPP/R1BQ1RK1 w kq - 0 1",
        "4rrk1/1pp2ppp/p1np1n2/4p3/2BPP3/2N2N2/PPP2PPP/2KR3R w - - 0 1",
    };

    constexpr int kBenchDepth = 13;

    stopSearch();
    tt.clear();

    auto start = std::chrono::steady_clock::now();
    int64_t totalNodes = 0;

    std::cout << "info string bench signature expected="
              << BENCH_SIGNATURE << " depth=" << kBenchDepth
              << " positions=" << (sizeof(kBenchFens) / sizeof(kBenchFens[0])) << std::endl;

    int posIdx = 0;
    for (const char* fen : kBenchFens) {
        Position pos;
        pos.setFromFen(fen);

        Search benchSearcher(tt);
        SearchLimits limits;
        limits.depth = kBenchDepth;
        benchSearcher.go(pos, limits, false);
        int64_t posNodes = benchSearcher.lastNodes();
        totalNodes += posNodes;
        Move bestMove = benchSearcher.lastBestMove();

        std::cout << "info string bench position " << ++posIdx
                  << " nodes=" << posNodes
                  << " bestmove=" << (bestMove ? bestMove.toUCI() : "0000")
                  << std::endl;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    int64_t nps = elapsed > 0 ? (totalNodes * 1000) / elapsed : totalNodes;

    if (totalNodes != BENCH_SIGNATURE) {
        std::cout << "info string BENCH_SIGNATURE_MISMATCH expected="
                  << BENCH_SIGNATURE << " got=" << totalNodes << std::endl;
    }

    std::cout << "Nodes: " << totalNodes << "  NPS: " << nps << std::endl;
}

// PGO training workload. Wider position set + deeper search than bench so the
// compiler sees branch counts on recursive negamax and qsearch, not just the
// UCI/setup paths. Not user-facing; called only by the build script during the
// profile-generation pass.
void runPgoTrain() {
    static const char* kPgoFens[] = {
        // 16 existing bench positions (proven-valid, varied phases)
        StartPosFen,
        "r3k2r/p1ppqpb1/bn2pnp1/2pP4/1p2P3/2N2N2/PPQBBPPP/R3K2R w KQkq - 0 1",
        "4rrk1/2p1qppp/p1np1n2/1p2p3/4P3/1NN1BP2/PPP1Q1PP/2KR3R w - - 0 1",
        "r3q1k1/1pp2ppp/p1npbn2/4p3/4P3/1NN1BP2/PPP2QPP/2KR3R w - - 0 1",
        "2r2rk1/pp1n1ppp/2pbpn2/q7/3P4/2N1PN2/PPQ1BPPP/2RR2K1 w - - 0 1",
        "r1bq1rk1/pppn1ppp/3bpn2/3p4/3P4/2N1PN2/PPQ1BPPP/R1BR2K1 w - - 0 1",
        "8/2p5/3p4/kP1P4/3K4/8/8/8 w - - 0 1",
        "8/8/2k5/2Pp4/3K4/8/8/8 w - d6 0 1",
        "rnbq1rk1/pp3ppp/2p1pn2/3p4/3P4/2N1PN2/PPQ1BPPP/R1B1K2R w KQ - 0 1",
        "r1bq1rk1/ppp2ppp/2np1n2/4p3/2BPP3/2N2N2/PPP2PPP/R1BQ1RK1 b - - 0 1",
        "2r2rk1/1bqn1ppp/p3pn2/1p6/3P4/1BN1PN2/PPQ2PPP/2RR2K1 w - - 0 1",
        "r4rk1/1pp1qppp/p1np1n2/4p3/2BPP3/2N2N2/PPP2PPP/R2Q1RK1 w - - 0 1",
        "3r2k1/5ppp/1p2pn2/p1r5/P1P5/1P1R1NP1/5PBP/3R2K1 w - - 0 1",
        "6k1/5ppp/1p2pn2/p1r5/P1P5/1P1R1NP1/5PBP/3R2K1 b - - 0 1",
        "r1bqk2r/ppp2ppp/2np1n2/4p3/2BPP3/2N2N2/PPP2PPP/R1BQ1RK1 w kq - 0 1",
        "4rrk1/1pp2ppp/p1np1n2/4p3/2BPP3/2N2N2/PPP2PPP/2KR3R w - - 0 1",

        // Openings (varied 1.e4 / 1.d4 / 1.c4 lines)
        "rnbqkbnr/pp1ppppp/8/2p5/4P3/5N2/PPPP1PPP/RNBQKBNR b KQkq - 1 2",
        "r1bqkbnr/pp1ppppp/2n5/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
        "rnbqkb1r/pp3ppp/2p1pn2/3p4/2PP4/5N2/PP2PPPP/RNBQKB1R w KQkq - 0 5",
        "rnbqkb1r/ppp1pppp/5n2/3p4/2PP4/2N5/PP2PPPP/R1BQKBNR b KQkq - 2 3",
        "r1bqkb1r/pppp1ppp/2n2n2/4p3/4P3/2N2N2/PPPP1PPP/R1BQKB1R w KQkq - 4 4",
        "rnbq1rk1/pp2ppbp/3p1np1/2pP4/4P3/2N2N2/PP2BPPP/R1BQK2R w KQ - 0 6",
        "rnbqk2r/pppp1ppp/4pn2/8/1bPP4/2N5/PP2PPPP/R1BQKBNR w KQkq - 2 4",

        // Sharp tactical middlegames (deep qsearch + extensions)
        "r2qrbk1/1ppb1p1p/pn4p1/3P1n2/2P2N2/2N1B3/PP3PPP/R2QRBK1 w - - 0 1",
        "2rr3k/pp3pp1/1nnqbN1p/3pN3/2pP4/2P3Q1/PPB4P/R4RK1 w - - 0 1",
        "1k1r4/pp1b1R2/3q2pp/4p3/2B5/4Q3/PPP2B1P/2K5 b - - 0 1",
        "3r1k2/4npp1/1ppr3p/p6P/P2PPPP1/1NR5/5K2/2R5 w - - 0 1",
        "r1b1qrk1/pp1nbppp/2p1pn2/3p4/2PP4/1PNBPN2/PB3PPP/R2QR1K1 w - - 0 11",
        "r2q1rk1/1b1nbppp/p2ppn2/1p6/3NP3/1BN1BP2/PPPQ2PP/2KR3R w - - 0 13",

        // Closed / locked-pawn structures (mobility + king-safety paths)
        "r1bq1rk1/2p1bppp/p1np1n2/1p2p3/4P3/PBNP1N1P/1PP2PP1/R1BQR1K1 b - - 0 1",
        "r1b2rk1/pp2bppp/2nq1n2/2pp4/3P4/2PBPN2/PP3PPP/RNBQR1K1 w - - 0 1",

        // Endgames (basic + instructive; exercises classical eval on sparse boards)
        "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1",
        "4k3/8/8/2pP4/2P5/8/8/4K3 w - - 0 1",
        "1R6/4kp2/4p3/4Pp1p/3P3P/3P4/r7/5K2 b - - 0 1",
        "8/p3kp2/1p4p1/8/PP6/2P3P1/4K3/8 w - - 0 1",
        "8/3rkp2/4p3/p3P3/2P5/2N2K2/Pn3P2/3R4 w - - 0 1",
        "5r2/2p3pk/1p1p1p1p/3P1n2/PPB4P/3R2P1/4Q1K1/8 b - - 0 36",
    };

    constexpr int kPgoDepth = 14;
    constexpr int64_t kPgoNodesPerPosition = 250000;

    stopSearch();
    tt.clear();

    auto start = std::chrono::steady_clock::now();
    int64_t totalNodes = 0;

    std::cout << "info string pgo-train begin depth=" << kPgoDepth
              << " nodes_per_position=" << kPgoNodesPerPosition
              << " positions=" << (sizeof(kPgoFens) / sizeof(kPgoFens[0])) << std::endl;

    int posIdx = 0;
    for (const char* fen : kPgoFens) {
        // Clear TT between unrelated positions so stale bestmoves from prior
        // searches cannot be misapplied across unrelated roots.
        tt.clear();

        Position p;
        p.setFromFen(fen);
        Color us = p.sideToMove();
        if (p.kingSq(us) == NO_SQUARE ||
            p.kingSq(~us) == NO_SQUARE ||
            p.isSquareAttacked(p.kingSq(~us), us)) {
            std::cout << "info string pgo-train skip illegal position "
                      << (posIdx + 1) << std::endl;
            ++posIdx;
            continue;
        }

        Search trainer(tt);
        SearchLimits limits;
        limits.depth = kPgoDepth;
        limits.nodes = kPgoNodesPerPosition;
        trainer.go(p, limits, false);
        int64_t posNodes = trainer.lastNodes();
        totalNodes += posNodes;

        std::cout << "info string pgo-train position " << ++posIdx
                  << " nodes=" << posNodes << std::endl;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    int64_t nps = elapsed > 0 ? (totalNodes * 1000) / elapsed : totalNodes;

    std::cout << "info string pgo-train done nodes=" << totalNodes
              << " elapsed_ms=" << elapsed << " nps=" << nps << std::endl;
}

} // namespace

void loop() {
    Position pos;
    pos.setFromFen(StartPosFen);

    searcher = new Search(tt);

    static StateInfo stateStack[1024];
    int stateIdx = 0;

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;

        if (cmd == "uci") {
            std::cout << "id name Rinnegan v7" << std::endl;
            std::cout << "id author Lorenzo" << std::endl;
            std::cout << "option name Hash type spin default 16 min 1 max 65536" << std::endl;
            std::cout << "option name Clear Hash type button" << std::endl;
            std::cout << "option name Threads type spin default 1 min 1 max 256" << std::endl;
            std::cout << "option name MoveOverhead type spin default 25 min 0 max 5000" << std::endl;
            std::cout << "option name PolicyFile type string default data/policy/stockfish-200k/train/smoke-policy.bin" << std::endl;
            std::cout << "option name UsePolicy type check default false" << std::endl;
            std::cout << "option name PolicyScale type spin default 160 min -10000 max 10000" << std::endl;
            std::cout << "option name PolicyTimeMod type check default false" << std::endl;
            std::cout << "option name PolicyTimeMargin type spin default 150 min 1 max 5000" << std::endl;
            std::cout << "option name PolicyTimeBoost type spin default 20 min 0 max 80" << std::endl;
            std::cout << "option name PolicyMode type spin default 1 min 0 max 2" << std::endl;
            std::cout << "option name UsePolicyV2 type check default false" << std::endl;
            std::cout << "option name PolicyV2File type string default data/policy/v2/policy.bin" << std::endl;
            std::cout << "option name PolicyV2Mode type spin default 2 min 0 max 2" << std::endl;
            std::cout << "option name PolicyV2Scale type spin default 160 min -10000 max 10000" << std::endl;
            std::cout << "option name PolicyV2QuietScale type spin default 160 min -10000 max 10000" << std::endl;
            std::cout << "option name PolicyV2EndgameScale type spin default 100 min 0 max 100" << std::endl;
            std::cout << "option name PolicyV2BonusClamp type spin default 20000 min 1000 max 60000" << std::endl;
            std::cout << "uciok" << std::endl;
        }
        else if (cmd == "isready") {
            std::cout << "readyok" << std::endl;
        }
        else if (cmd == "ucinewgame") {
            stopSearch();
            tt.clear();
            pos.setFromFen(StartPosFen);
            stateIdx = 0;
        }
        else if (cmd == "setoption") {
            std::string name, value;
            parseSetOption(line, name, value);

            if (name == "Hash" && !value.empty()) {
                tt.resize(std::stoi(value));
            } else if (name == "Clear Hash") {
                tt.clear();
            } else if (name == "Threads" && !value.empty()) {
                threads = std::clamp(std::stoi(value), 1, 256);
            } else if (name == "MoveOverhead" && !value.empty()) {
                moveOverhead = std::clamp(std::stoi(value), 0, 5000);
            } else if (name == "PolicyFile" && !value.empty()) {
                policyFile = value;
                bool loaded = Policy::load(policyFile);
                reportPolicyLoad(policyFile, loaded);
                tt.clear();
            } else if (name == "UsePolicy" && !value.empty()) {
                bool newValue = (value == "true" || value == "1");
                if (newValue && !Policy::isLoaded())
                    reportPolicyLoad(policyFile, Policy::load(policyFile));
                if (newValue != usePolicy) {
                    usePolicy = newValue;
                    tt.clear();
                }
            } else if (name == "PolicyScale" && !value.empty()) {
                policyScale = std::clamp(std::stoi(value), -10000, 10000);
                tt.clear();
            } else if (name == "PolicyTimeMod" && !value.empty()) {
                policyTimeMod = (value == "true" || value == "1");
            } else if (name == "PolicyTimeMargin" && !value.empty()) {
                policyTimeMargin = std::clamp(std::stoi(value), 1, 5000);
            } else if (name == "PolicyTimeBoost" && !value.empty()) {
                policyTimeBoost = std::clamp(std::stoi(value), 0, 80);
            } else if (name == "PolicyMode" && !value.empty()) {
                policyMode = std::clamp(std::stoi(value), 0, 2);
                tt.clear();
            } else if (name == "UsePolicyV2" && !value.empty()) {
                bool newValue = (value == "true" || value == "1");
                if (newValue && !Policy::isV2Loaded())
                    reportPolicyLoad(policyV2File, Policy::loadV2(policyV2File));
                if (newValue != usePolicyV2) {
                    usePolicyV2 = newValue;
                    tt.clear();
                }
            } else if (name == "PolicyV2File" && !value.empty()) {
                policyV2File = value;
                bool loaded = Policy::loadV2(policyV2File);
                reportPolicyLoad(policyV2File, loaded);
                tt.clear();
            } else if (name == "PolicyV2Mode" && !value.empty()) {
                policyV2Mode = std::clamp(std::stoi(value), 0, 2);
                tt.clear();
            } else if (name == "PolicyV2Scale" && !value.empty()) {
                policyV2Scale = std::clamp(std::stoi(value), -10000, 10000);
                tt.clear();
            } else if (name == "PolicyV2QuietScale" && !value.empty()) {
                policyV2QuietScale = std::clamp(std::stoi(value), -10000, 10000);
                tt.clear();
            } else if (name == "PolicyV2EndgameScale" && !value.empty()) {
                policyV2EndgameScale = std::clamp(std::stoi(value), 0, 100);
                tt.clear();
            } else if (name == "PolicyV2BonusClamp" && !value.empty()) {
                policyV2BonusClamp = std::clamp(std::stoi(value), 1000, 60000);
                tt.clear();
            }
        }
        else if (cmd == "position") {
            stopSearch();

            std::string token;
            ss >> token;
            stateIdx = 0;

            if (token == "startpos") {
                pos.setFromFen(StartPosFen);
                ss >> token;
            } else if (token == "fen") {
                std::string fen;
                bool sawMoves = false;
                for (int i = 0; i < 6 && ss >> token; ++i) {
                    if (token == "moves") {
                        sawMoves = true;
                        break;
                    }
                    if (!fen.empty()) fen += ' ';
                    fen += token;
                }
                pos.setFromFen(fen);
                if (!sawMoves) {
                    if (!(ss >> token)) continue;
                    if (token != "moves") continue;
                }
            }

            while (ss >> token) {
                if (token == "moves") continue;
                Move move = parseMove(pos, token);
                if (move)
                    pos.makeMove(move, stateStack[stateIdx++]);
            }
        }
        else if (cmd == "go") {
            SearchLimits limits;
            std::vector<std::string> tokens;
            std::string token;
            while (ss >> token)
                tokens.push_back(token);

            for (size_t i = 0; i < tokens.size(); ++i) {
                token = tokens[i];
                auto nextInt = [&]() -> int {
                    if (i + 1 >= tokens.size()) return 0;
                    return std::stoi(tokens[++i]);
                };

                if (token == "searchmoves") {
                    while (i + 1 < tokens.size() && !isGoKeyword(tokens[i + 1])) {
                        Move move = parseMove(pos, tokens[++i]);
                        if (move && limits.searchMoveCount < MAX_MOVES)
                            limits.searchMoves[limits.searchMoveCount++] = move;
                    }
                } else if (token == "depth") limits.depth = nextInt();
                else if (token == "nodes") limits.nodes = nextInt();
                else if (token == "movetime") limits.movetime = nextInt();
                else if (token == "wtime") limits.wtime = nextInt();
                else if (token == "btime") limits.btime = nextInt();
                else if (token == "winc") limits.winc = nextInt();
                else if (token == "binc") limits.binc = nextInt();
                else if (token == "movestogo") limits.movestogo = nextInt();
                else if (token == "infinite") limits.infinite = true;
            }

            stopSearch();
            delete searcher;
            searcher = new Search(tt);
            searchThread = std::thread([&pos, limits]() {
                searcher->go(pos, limits);
            });
        }
        else if (cmd == "stop") {
            stopSearch();
        }
        else if (cmd == "perft") {
            stopSearch();
            int depth = 6;
            ss >> depth;
            Perft::divide(pos, depth);
        }
        else if (cmd == "eval") {
            stopSearch();
            int classical = Eval::evaluateClassical(pos);
            std::cout << "info string classical_eval cp " << classical << std::endl;
        }
        else if (cmd == "evaltrace") {
            stopSearch();
            std::cout << Eval::trace(pos) << std::endl;
        }
        else if (cmd == "bench") {
            stopSearch();
            runBench();
        }
        else if (cmd == "policystats") {
            // Latency telemetry. Reports total ns spent inside the policy
            // root scoring call and the call count, then resets. Used by
            // scripts/bench_policy_v2.sh and tools/policy/v2/bench_policy.py.
            std::string sub;
            ss >> sub;
            auto p = Policy::readPerfCounters();
            std::cout << "info string policy_calls=" << p.calls
                      << " feature_ns=" << p.featureNanos
                      << " forward_ns=" << p.forwardNanos
                      << " saturated=" << p.saturatedBonuses
                      << " scored_moves=" << p.scoredMoves
                      << std::endl;
            if (sub == "reset")
                Policy::resetPerfCounters();
        }
        else if (cmd == "pgo-train") {
            stopSearch();
            runPgoTrain();
        }
        else if (cmd == "quit") {
            stopSearch();
            break;
        }
    }

    stopSearch();
    delete searcher;
    searcher = nullptr;
}

} // namespace UCI
