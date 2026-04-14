#pragma once

#include <cstdint>
#include <string>
#include <cassert>

using Bitboard = uint64_t;

enum Color : int { WHITE, BLACK, NO_COLOR };

constexpr Color operator~(Color c) { return Color(c ^ 1); }

enum PieceType : int {
    PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING, NO_PIECE_TYPE
};

enum Piece : int {
    W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
    B_PAWN = 8, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING,
    NO_PIECE = 15
};

constexpr Piece makePiece(Color c, PieceType pt) {
    return Piece((c << 3) | pt);
}
constexpr Color pieceColor(Piece p) { return Color(p >> 3); }
constexpr PieceType pieceType(Piece p) { return PieceType(p & 7); }

enum Square : int {
    A1, B1, C1, D1, E1, F1, G1, H1,
    A2, B2, C2, D2, E2, F2, G2, H2,
    A3, B3, C3, D3, E3, F3, G3, H3,
    A4, B4, C4, D4, E4, F4, G4, H4,
    A5, B5, C5, D5, E5, F5, G5, H5,
    A6, B6, C6, D6, E6, F6, G6, H6,
    A7, B7, C7, D7, E7, F7, G7, H7,
    A8, B8, C8, D8, E8, F8, G8, H8,
    NO_SQUARE = 64
};

constexpr int SQUARE_NB = 64;

constexpr Square makeSquare(int file, int rank) {
    return Square(rank * 8 + file);
}
constexpr int fileOf(Square s) { return s & 7; }
constexpr int rankOf(Square s) { return s >> 3; }

constexpr Square operator+(Square s, int d) { return Square(int(s) + d); }
constexpr Square operator-(Square s, int d) { return Square(int(s) - d); }
inline Square& operator++(Square& s) { return s = Square(int(s) + 1); }

// Move encoding: bits 0-5: from, 6-11: to, 12-13: promo piece, 14-15: flag
enum MoveFlag : uint16_t {
    FLAG_NORMAL    = 0,
    FLAG_PROMOTION = 1 << 14,
    FLAG_ENPASSANT = 2 << 14,
    FLAG_CASTLING  = 3 << 14,
};

struct Move {
    uint16_t data = 0;

    Move() = default;
    constexpr Move(uint16_t d) : data(d) {}

    static constexpr Move make(Square from, Square to, MoveFlag flag = FLAG_NORMAL, PieceType promo = KNIGHT) {
        return Move(uint16_t(from) | (uint16_t(to) << 6) | (uint16_t(promo - KNIGHT) << 12) | uint16_t(flag));
    }

    constexpr Square from() const { return Square(data & 0x3F); }
    constexpr Square to() const { return Square((data >> 6) & 0x3F); }
    constexpr PieceType promoPiece() const { return PieceType(((data >> 12) & 3) + KNIGHT); }
    constexpr MoveFlag flag() const { return MoveFlag(data & 0xC000); }

    constexpr bool operator==(Move o) const { return data == o.data; }
    constexpr bool operator!=(Move o) const { return data != o.data; }
    constexpr explicit operator bool() const { return data != 0; }

    std::string toUCI() const {
        std::string s;
        s += char('a' + fileOf(from()));
        s += char('1' + rankOf(from()));
        s += char('a' + fileOf(to()));
        s += char('1' + rankOf(to()));
        if (flag() == FLAG_PROMOTION) {
            constexpr char promoChar[] = "nbrq";
            s += promoChar[promoPiece() - KNIGHT];
        }
        return s;
    }
};

constexpr Move MOVE_NONE = Move(0);

// Castling rights bitmask
enum CastlingRight : int {
    NO_CASTLING  = 0,
    WHITE_OO     = 1,
    WHITE_OOO    = 2,
    BLACK_OO     = 4,
    BLACK_OOO    = 8,
    ALL_CASTLING = 15,
};

constexpr CastlingRight operator|(CastlingRight a, CastlingRight b) { return CastlingRight(int(a) | int(b)); }
constexpr CastlingRight operator&(CastlingRight a, CastlingRight b) { return CastlingRight(int(a) & int(b)); }
constexpr CastlingRight operator~(CastlingRight a) { return CastlingRight(~int(a) & 15); }
inline CastlingRight& operator|=(CastlingRight& a, CastlingRight b) { return a = a | b; }
inline CastlingRight& operator&=(CastlingRight& a, CastlingRight b) { return a = a & b; }

// State info for make/unmake
struct StateInfo {
    CastlingRight castling = NO_CASTLING;
    Square epSquare = NO_SQUARE;
    int halfmoveClock = 0;
    Piece captured = NO_PIECE;
    uint64_t key = 0;
    StateInfo* previous = nullptr;
};

// Score
constexpr int SCORE_NONE  = -32001;
constexpr int SCORE_DRAW  = 0;
constexpr int SCORE_MATE  = 32000;
constexpr int SCORE_INF   = 32001;
constexpr int SCORE_MATE_IN_MAX_PLY = SCORE_MATE - 256;

constexpr int MAX_PLY   = 128;
constexpr int MAX_MOVES = 256;

// File and rank masks
constexpr Bitboard FileABB = 0x0101010101010101ULL;
constexpr Bitboard FileBBB = FileABB << 1;
constexpr Bitboard FileCBB = FileABB << 2;
constexpr Bitboard FileDBB = FileABB << 3;
constexpr Bitboard FileEBB = FileABB << 4;
constexpr Bitboard FileFBB = FileABB << 5;
constexpr Bitboard FileGBB = FileABB << 6;
constexpr Bitboard FileHBB = FileABB << 7;

constexpr Bitboard Rank1BB = 0xFFULL;
constexpr Bitboard Rank2BB = Rank1BB << 8;
constexpr Bitboard Rank3BB = Rank1BB << 16;
constexpr Bitboard Rank4BB = Rank1BB << 24;
constexpr Bitboard Rank5BB = Rank1BB << 32;
constexpr Bitboard Rank6BB = Rank1BB << 40;
constexpr Bitboard Rank7BB = Rank1BB << 48;
constexpr Bitboard Rank8BB = Rank1BB << 56;

constexpr Bitboard rankBB(int r) { return Rank1BB << (8 * r); }
constexpr Bitboard fileBB(int f) { return FileABB << f; }
constexpr Bitboard squareBB(Square s) { return 1ULL << s; }
