#include "uci.h"
#include "position.h"
#include "search.h"
#include "tt.h"
#include "perft.h"
#include "movegen.h"
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace UCI {

static TranspositionTable tt;
static Search* searcher = nullptr;
static std::thread searchThread;

static Move parseMove(Position& pos, const std::string& str) {
    if (str.length() < 4) return MOVE_NONE;

    Square from = makeSquare(str[0] - 'a', str[1] - '1');
    Square to   = makeSquare(str[2] - 'a', str[3] - '1');

    MoveList moves;
    MoveGen::generateLegal(pos, moves);

    for (int i = 0; i < moves.count; ++i) {
        Move m = moves[i];
        if (m.from() == from && m.to() == to) {
            if (m.flag() == FLAG_PROMOTION) {
                if (str.length() >= 5) {
                    char promo = str[4];
                    PieceType pt = QUEEN;
                    switch (promo) {
                        case 'n': pt = KNIGHT; break;
                        case 'b': pt = BISHOP; break;
                        case 'r': pt = ROOK;   break;
                        case 'q': pt = QUEEN;  break;
                        default: break;
                    }
                    if (m.promoPiece() == pt) return m;
                } else {
                    // Default to queen
                    if (m.promoPiece() == QUEEN) return m;
                }
            } else {
                return m;
            }
        }
    }
    return MOVE_NONE;
}

void loop() {
    Position pos;
    pos.setFromFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

    searcher = new Search(tt);

    // Allocate StateInfo stack for moves
    static StateInfo stateStack[1024];
    int stateIdx = 0;

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;

        if (cmd == "uci") {
            std::cout << "id name Rinnegan v1" << std::endl;
            std::cout << "id author Lorenzo" << std::endl;
            std::cout << "option name Hash type spin default 16 min 1 max 1024" << std::endl;
            std::cout << "option name Threads type spin default 1 min 1 max 1" << std::endl;
            std::cout << "uciok" << std::endl;
        }
        else if (cmd == "isready") {
            std::cout << "readyok" << std::endl;
        }
        else if (cmd == "ucinewgame") {
            tt.clear();
            pos.setFromFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
            stateIdx = 0;
        }
        else if (cmd == "setoption") {
            std::string name, value;
            std::string token;
            ss >> token; // "name"
            ss >> name;
            ss >> token; // "value"
            ss >> value;
            if (name == "Hash") {
                int mb = std::stoi(value);
                tt.resize(mb);
            }
        }
        else if (cmd == "position") {
            std::string token;
            ss >> token;
            stateIdx = 0;

            if (token == "startpos") {
                pos.setFromFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
                ss >> token; // consume "moves" if present
            } else if (token == "fen") {
                std::string fen;
                // Read 6 FEN parts
                for (int i = 0; i < 6 && ss >> token; ++i) {
                    if (token == "moves") { break; }
                    if (!fen.empty()) fen += ' ';
                    fen += token;
                }
                pos.setFromFen(fen);
                if (token != "moves") continue;
            }

            // Apply moves
            while (ss >> token) {
                if (token == "moves") continue;
                Move m = parseMove(pos, token);
                if (m.data) {
                    pos.makeMove(m, stateStack[stateIdx++]);
                }
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

            // Run search in separate thread
            if (searchThread.joinable()) searchThread.join();
            delete searcher;
            searcher = new Search(tt);
            searchThread = std::thread([&pos, limits]() {
                searcher->go(pos, limits);
            });
        }
        else if (cmd == "stop") {
            if (searcher) searcher->stop();
            if (searchThread.joinable()) searchThread.join();
        }
        else if (cmd == "perft") {
            int depth = 6;
            ss >> depth;
            Perft::divide(pos, depth);
        }
        else if (cmd == "quit") {
            if (searcher) searcher->stop();
            if (searchThread.joinable()) searchThread.join();
            break;
        }
    }

    delete searcher;
}

} // namespace UCI
