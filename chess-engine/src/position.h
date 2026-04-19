#pragma once

#include "types.h"
#include <string>
#include <vector>

class Position {
public:
    Position();

    void setFromFen(const std::string& fen);
    void setFromFen(const std::string& fen, StateInfo& rootState);
    void copyFrom(const Position& other, StateInfo& rootState);
    std::string fen() const;

    // Board queries
    Color sideToMove() const { return side; }
    Bitboard pieces(Color c) const { return byColor[c]; }
    Bitboard pieces(Color c, PieceType pt) const { return byType[pt] & byColor[c]; }
    Bitboard pieces(PieceType pt) const { return byType[pt]; }
    Bitboard pieces(PieceType pt1, PieceType pt2) const { return byType[pt1] | byType[pt2]; }
    Bitboard allPieces() const { return byColor[WHITE] | byColor[BLACK]; }
    Piece pieceOn(Square s) const { return mailbox[s]; }
    Square kingSq(Color c) const;

    // Attack detection
    bool isSquareAttacked(Square s, Color by) const;
    Bitboard attackersTo(Square s, Bitboard occ) const;
    bool inCheck() const;

    // Move execution
    void makeMove(Move m, StateInfo& newSt);
    void unmakeMove(Move m);
    void doNullMove(StateInfo& newSt);
    void undoNullMove();

    // State
    CastlingRight castlingRights() const { return st->castling; }
    Square epSquare() const { return st->epSquare; }
    int halfmoveClock() const { return st->halfmoveClock; }
    uint64_t key() const { return st->key; }

    // For repetition detection
    bool isRepetition(int ply) const;
    bool isDraw(int ply) const;

    // Fullmove counter
    int fullmoveNumber = 1;

    // Key history for repetition
    std::vector<uint64_t> keyHistory;

    // State info pointer
    StateInfo* st = nullptr;

private:
    void putPiece(Piece p, Square s);
    void removePiece(Square s);
    void movePiece(Square from, Square to);
    uint64_t computeKey() const;

    Bitboard byType[7] = {};  // indexed by PieceType
    Bitboard byColor[2] = {};
    Piece mailbox[64];
    Color side = WHITE;
};
