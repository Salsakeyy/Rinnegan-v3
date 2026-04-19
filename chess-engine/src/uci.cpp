#include "uci.h"
#include "nnue.h"
#include "movegen.h"
#include "perft.h"
#include "position.h"
#include "search.h"
#include "tt.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace UCI {

bool useNNUE = true;
int threads = 1;
std::string evalFile = "rinnegan-v4.net";

namespace {

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

void reportNnueLoad(const std::string& path, bool loaded) {
    if (loaded) {
        std::cout << "info string NNUE loaded from " << path << std::endl;
        return;
    }

    std::error_code ec;
    if (std::filesystem::exists(path, ec) &&
        std::filesystem::is_regular_file(path, ec) &&
        std::filesystem::file_size(path, ec) != NNUE::expectedFileSize() &&
        std::filesystem::file_size(path, ec) != NNUE::expectedPaddedFileSize()) {
        std::cout << "info string NNUE: wrong net size, falling back to PeSTO" << std::endl;
    } else {
        std::cout << "info string NNUE load failed, using PeSTO" << std::endl;
    }
}

void tryLoadDefaultNet() {
    NNUE::load(evalFile);
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

    for (const char* fen : kBenchFens) {
        Position pos;
        pos.setFromFen(fen);

        Search benchSearcher(tt);
        SearchLimits limits;
        limits.depth = kBenchDepth;
        benchSearcher.go(pos, limits, false);
        totalNodes += benchSearcher.lastNodes();
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    int64_t nps = elapsed > 0 ? (totalNodes * 1000) / elapsed : totalNodes;

    std::cout << "Nodes: " << totalNodes << "  NPS: " << nps << std::endl;
}

} // namespace

void loop() {
    Position pos;
    pos.setFromFen(StartPosFen);

    tryLoadDefaultNet();
    searcher = new Search(tt);

    static StateInfo stateStack[1024];
    int stateIdx = 0;

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;

        if (cmd == "uci") {
            std::cout << "id name Rinnegan v4" << std::endl;
            std::cout << "id author Lorenzo" << std::endl;
            std::cout << "option name Hash type spin default 16 min 1 max 65536" << std::endl;
            std::cout << "option name Threads type spin default 1 min 1 max 256" << std::endl;
            std::cout << "option name EvalFile type string default rinnegan-v4.net" << std::endl;
            std::cout << "option name UseNNUE type check default true" << std::endl;
            if (NNUE::isLoaded())
                std::cout << "info string NNUE loaded from " << NNUE::loadedPath() << std::endl;
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
            } else if (name == "Threads" && !value.empty()) {
                threads = std::clamp(std::stoi(value), 1, 256);
            } else if (name == "EvalFile" && !value.empty()) {
                evalFile = value;
                bool loaded = NNUE::load(evalFile);
                reportNnueLoad(evalFile, loaded);
                tt.clear();
            } else if (name == "UseNNUE" && !value.empty()) {
                bool newValue = (value == "true" || value == "1");
                if (newValue != useNNUE) {
                    useNNUE = newValue;
                    tt.clear();
                }
            }
        }
        else if (cmd == "position") {
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
            std::string token;
            while (ss >> token) {
                if (token == "depth") ss >> limits.depth;
                else if (token == "nodes") ss >> limits.nodes;
                else if (token == "movetime") ss >> limits.movetime;
                else if (token == "wtime") ss >> limits.wtime;
                else if (token == "btime") ss >> limits.btime;
                else if (token == "winc") ss >> limits.winc;
                else if (token == "binc") ss >> limits.binc;
                else if (token == "movestogo") ss >> limits.movestogo;
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
            int depth = 6;
            ss >> depth;
            Perft::divide(pos, depth);
        }
        else if (cmd == "bench") {
            runBench();
        }
        else if (cmd == "quit") {
            stopSearch();
            break;
        }
    }

    delete searcher;
}

} // namespace UCI
