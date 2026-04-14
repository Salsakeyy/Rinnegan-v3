#include "position.h"
#include "bitboard.h"
#include "zobrist.h"
#include <sstream>
#include <cstring>
#include <algorithm>

// Castling rights update table: castlingMask[sq] is ANDed with castling rights on any move from/to sq
static CastlingRight castlingMask[64];

static void initCastlingMask() {
    for (int i = 0; i < 64; ++i) castlingMask[i] = ALL_CASTLING;
    castlingMask[E1] = CastlingRight(~(WHITE_OO | WHITE_OOO) & 15);
    castlingMask[H1] = CastlingRight(~WHITE_OO & 15);
    castlingMask[A1] = CastlingRight(~WHITE_OOO & 15);
    castlingMask[E8] = CastlingRight(~(BLACK_OO | BLACK_OOO) & 15);
    castlingMask[H8] = CastlingRight(~BLACK_OO & 15);
    castlingMask[A8] = CastlingRight(~BLACK_OOO & 15);
}

static bool castlingMaskInited = false;

Position::Position() {
    if (!castlingMaskInited) {
        initCastlingMask();
        castlingMaskInited = true;
    }
    std::fill(std::begin(mailbox), std::end(mailbox), NO_PIECE);
}

void Position::putPiece(Piece p, Square s) {
    mailbox[s] = p;
    Bitboard bb = squareBB(s);
    byType[pieceType(p)] |= bb;
    byColor[pieceColor(p)] |= bb;
}

void Position::removePiece(Square s) {
    Piece p = mailbox[s];
    Bitboard bb = squareBB(s);
    byType[pieceType(p)] ^= bb;
    byColor[pieceColor(p)] ^= bb;
    mailbox[s] = NO_PIECE;
}

void Position::movePiece(Square from, Square to) {
    Piece p = mailbox[from];
    Bitboard fromTo = squareBB(from) | squareBB(to);
    byType[pieceType(p)] ^= fromTo;
    byColor[pieceColor(p)] ^= fromTo;
    mailbox[from] = NO_PIECE;
    mailbox[to] = p;
}

uint64_t Position::computeKey() const {
    uint64_t k = 0;
    for (int sq = 0; sq < 64; ++sq) {
        if (mailbox[sq] != NO_PIECE)
            k ^= Zobrist::PieceSquare[mailbox[sq]][sq];
    }
    k ^= Zobrist::Castling[st->castling];
    if (st->epSquare != NO_SQUARE)
        k ^= Zobrist::EnPassant[fileOf(st->epSquare)];
    if (side == BLACK)
        k ^= Zobrist::SideToMove;
    return k;
}

void Position::setFromFen(const std::string& fen) {
    std::fill(std::begin(mailbox), std::end(mailbox), NO_PIECE);
    std::memset(byType, 0, sizeof(byType));
    std::memset(byColor, 0, sizeof(byColor));
    keyHistory.clear();

    std::istringstream ss(fen);
    std::string board, sideStr, castleStr, epStr;
    int hmc = 0, fmc = 1;

    ss >> board >> sideStr >> castleStr >> epStr;
    if (ss >> hmc) { ss >> fmc; }

    // Parse board
    int sq = 56; // start from A8
    for (char c : board) {
        if (c == '/') {
            sq -= 16; // go to start of next rank down
        } else if (c >= '1' && c <= '8') {
            sq += (c - '0');
        } else {
            Color color = (c >= 'a') ? BLACK : WHITE;
            char lower = (c >= 'a') ? c : (c + 32);
            PieceType pt = NO_PIECE_TYPE;
            switch (lower) {
                case 'p': pt = PAWN;   break;
                case 'n': pt = KNIGHT; break;
                case 'b': pt = BISHOP; break;
                case 'r': pt = ROOK;   break;
                case 'q': pt = QUEEN;  break;
                case 'k': pt = KING;   break;
                default: break;
            }
            if (pt != NO_PIECE_TYPE)
                putPiece(makePiece(color, pt), Square(sq));
            ++sq;
        }
    }

    side = (sideStr == "b") ? BLACK : WHITE;

    // We need a StateInfo
    static StateInfo rootSt;
    rootSt = StateInfo{};
    st = &rootSt;

    // Castling
    st->castling = NO_CASTLING;
    if (castleStr != "-") {
        for (char c : castleStr) {
            switch (c) {
                case 'K': st->castling |= WHITE_OO; break;
                case 'Q': st->castling |= WHITE_OOO; break;
                case 'k': st->castling |= BLACK_OO; break;
                case 'q': st->castling |= BLACK_OOO; break;
                default: break;
            }
        }
    }

    // En passant
    if (epStr != "-" && epStr.length() == 2) {
        int f = epStr[0] - 'a';
        int r = epStr[1] - '1';
        st->epSquare = makeSquare(f, r);
    } else {
        st->epSquare = NO_SQUARE;
    }

    st->halfmoveClock = hmc;
    fullmoveNumber = fmc;

    st->key = computeKey();
    keyHistory.push_back(st->key);
}

std::string Position::fen() const {
    std::string s;
    for (int r = 7; r >= 0; --r) {
        int empty = 0;
        for (int f = 0; f < 8; ++f) {
            Piece p = mailbox[r * 8 + f];
            if (p == NO_PIECE) {
                ++empty;
            } else {
                if (empty) { s += char('0' + empty); empty = 0; }
                char c;
                PieceType pt = pieceType(p);
                Color col = pieceColor(p);
                const char types[] = "pnbrqk";
                c = types[pt];
                if (col == WHITE) c -= 32;
                s += c;
            }
        }
        if (empty) s += char('0' + empty);
        if (r > 0) s += '/';
    }

    s += (side == WHITE) ? " w " : " b ";

    std::string castle;
    if (st->castling & WHITE_OO)  castle += 'K';
    if (st->castling & WHITE_OOO) castle += 'Q';
    if (st->castling & BLACK_OO)  castle += 'k';
    if (st->castling & BLACK_OOO) castle += 'q';
    if (castle.empty()) castle = "-";
    s += castle;

    s += ' ';
    if (st->epSquare != NO_SQUARE) {
        s += char('a' + fileOf(st->epSquare));
        s += char('1' + rankOf(st->epSquare));
    } else {
        s += '-';
    }

    s += ' ' + std::to_string(st->halfmoveClock);
    s += ' ' + std::to_string(fullmoveNumber);

    return s;
}

Square Position::kingSq(Color c) const {
    return BB::lsb(pieces(c, KING));
}

Bitboard Position::attackersTo(Square s, Bitboard occ) const {
    return (BB::PawnAttacks[BLACK][s] & pieces(WHITE, PAWN))
         | (BB::PawnAttacks[WHITE][s] & pieces(BLACK, PAWN))
         | (BB::KnightAttacks[s]      & pieces(KNIGHT))
         | (BB::KingAttacks[s]        & pieces(KING))
         | (BB::rookAttacks(s, occ)   & pieces(ROOK, QUEEN))
         | (BB::bishopAttacks(s, occ) & pieces(BISHOP, QUEEN));
}

bool Position::isSquareAttacked(Square s, Color by) const {
    Bitboard occ = allPieces();
    return (BB::PawnAttacks[~by][s] & pieces(by, PAWN))
         | (BB::KnightAttacks[s]    & pieces(by, KNIGHT))
         | (BB::KingAttacks[s]      & pieces(by, KING))
         | (BB::rookAttacks(s, occ) & (pieces(by, ROOK) | pieces(by, QUEEN)))
         | (BB::bishopAttacks(s, occ) & (pieces(by, BISHOP) | pieces(by, QUEEN)));
}

bool Position::inCheck() const {
    return isSquareAttacked(kingSq(side), ~side);
}

void Position::makeMove(Move m, StateInfo& newSt) {
    newSt = StateInfo{};
    newSt.previous = st;

    Square from = m.from();
    Square to = m.to();
    MoveFlag flag = m.flag();
    Piece pc = mailbox[from];
    Piece captured = (flag == FLAG_ENPASSANT) ? makePiece(~side, PAWN) : mailbox[to];

    newSt.castling = st->castling;
    newSt.epSquare = NO_SQUARE;
    newSt.halfmoveClock = st->halfmoveClock + 1;
    newSt.captured = captured;

    // Start with previous key
    uint64_t k = st->key ^ Zobrist::SideToMove;

    // Remove castling key, will re-add after update
    k ^= Zobrist::Castling[st->castling];

    // Handle en passant square from previous state
    if (st->epSquare != NO_SQUARE)
        k ^= Zobrist::EnPassant[fileOf(st->epSquare)];

    if (flag == FLAG_CASTLING) {
        // King already moves from/to; need to also move the rook
        Square rookFrom, rookTo;
        if (to > from) { // Kingside
            rookFrom = Square(from + 3);
            rookTo = Square(from + 1);
        } else { // Queenside
            rookFrom = Square(from - 4);
            rookTo = Square(from - 1);
        }
        k ^= Zobrist::PieceSquare[mailbox[rookFrom]][rookFrom];
        k ^= Zobrist::PieceSquare[mailbox[rookFrom]][rookTo];
        movePiece(rookFrom, rookTo);
    }

    // Handle captures
    if (captured != NO_PIECE) {
        Square capSq = to;
        if (flag == FLAG_ENPASSANT) {
            capSq = (side == WHITE) ? Square(to - 8) : Square(to + 8);
        }
        k ^= Zobrist::PieceSquare[captured][capSq];
        removePiece(capSq);
        newSt.halfmoveClock = 0;
    }

    // Move the piece
    k ^= Zobrist::PieceSquare[pc][from];
    k ^= Zobrist::PieceSquare[pc][to];
    movePiece(from, to);

    // Pawn-specific
    if (pieceType(pc) == PAWN) {
        newSt.halfmoveClock = 0;

        // Double pawn push: set ep square
        if (std::abs(int(to) - int(from)) == 16) {
            Square epSq = Square((int(from) + int(to)) / 2);
            // Only set ep if opponent pawn can actually capture
            if (BB::PawnAttacks[side][epSq] & pieces(~side, PAWN)) {
                newSt.epSquare = epSq;
                k ^= Zobrist::EnPassant[fileOf(epSq)];
            }
        }

        // Promotion
        if (flag == FLAG_PROMOTION) {
            Piece promoPc = makePiece(side, m.promoPiece());
            k ^= Zobrist::PieceSquare[pc][to];         // remove pawn
            k ^= Zobrist::PieceSquare[promoPc][to];     // add promoted piece
            removePiece(to);
            putPiece(promoPc, to);
        }
    }

    // Update castling rights
    newSt.castling &= castlingMask[from];
    newSt.castling &= castlingMask[to];
    k ^= Zobrist::Castling[newSt.castling];

    newSt.key = k;
    st = &newSt;

    // Switch side
    side = ~side;
    if (side == WHITE) fullmoveNumber++;

    // Record key for repetition
    keyHistory.push_back(k);
}

void Position::unmakeMove(Move m) {
    side = ~side;
    if (side == BLACK) fullmoveNumber--;

    keyHistory.pop_back();

    Square from = m.from();
    Square to = m.to();
    MoveFlag flag = m.flag();

    if (flag == FLAG_PROMOTION) {
        removePiece(to);
        putPiece(makePiece(side, PAWN), to);
    }

    // Move piece back
    movePiece(to, from);

    // Restore captured piece
    Piece captured = st->captured;
    if (captured != NO_PIECE) {
        Square capSq = to;
        if (flag == FLAG_ENPASSANT) {
            capSq = (side == WHITE) ? Square(to - 8) : Square(to + 8);
        }
        putPiece(captured, capSq);
    }

    // Undo castling rook move
    if (flag == FLAG_CASTLING) {
        Square rookFrom, rookTo;
        if (to > from) {
            rookFrom = Square(from + 3);
            rookTo = Square(from + 1);
        } else {
            rookFrom = Square(from - 4);
            rookTo = Square(from - 1);
        }
        movePiece(rookTo, rookFrom);
    }

    st = st->previous;
}

void Position::doNullMove(StateInfo& newSt) {
    newSt = StateInfo{};
    newSt.previous = st;
    newSt.castling = st->castling;
    newSt.epSquare = NO_SQUARE;
    newSt.halfmoveClock = st->halfmoveClock + 1;
    newSt.captured = NO_PIECE;

    uint64_t k = st->key ^ Zobrist::SideToMove;
    if (st->epSquare != NO_SQUARE)
        k ^= Zobrist::EnPassant[fileOf(st->epSquare)];
    newSt.key = k;

    st = &newSt;
    side = ~side;
    keyHistory.push_back(newSt.key);
}

void Position::undoNullMove() {
    keyHistory.pop_back();
    side = ~side;
    st = st->previous;
}

bool Position::isRepetition(int ply) const {
    int end = std::min(st->halfmoveClock, (int)keyHistory.size() - 1);
    int cnt = 0;
    for (int i = 4; i <= end; i += 2) {
        if (keyHistory[keyHistory.size() - 1 - i] == st->key) {
            if (i < ply) return true; // repetition within search
            if (++cnt >= 2) return true; // 3-fold
        }
    }
    return false;
}

bool Position::isDraw(int ply) const {
    if (st->halfmoveClock >= 100) return true;
    return isRepetition(ply);
}
