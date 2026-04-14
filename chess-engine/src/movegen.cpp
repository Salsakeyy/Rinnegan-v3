#include "movegen.h"
#include "bitboard.h"

namespace MoveGen {

static void addPawnMoves(MoveList& list, Square from, Square to, bool isCapture, Color side) {
    int toRank = rankOf(to);
    if (toRank == 7 || toRank == 0) {
        // Promotion
        list.add(Move::make(from, to, FLAG_PROMOTION, QUEEN));
        list.add(Move::make(from, to, FLAG_PROMOTION, ROOK));
        list.add(Move::make(from, to, FLAG_PROMOTION, BISHOP));
        list.add(Move::make(from, to, FLAG_PROMOTION, KNIGHT));
    } else {
        list.add(Move::make(from, to));
    }
    (void)isCapture;
    (void)side;
}

void generatePseudoLegal(const Position& pos, MoveList& list) {
    Color us = pos.sideToMove();
    Color them = ~us;
    Bitboard ourPieces = pos.pieces(us);
    Bitboard theirPieces = pos.pieces(them);
    Bitboard occ = pos.allPieces();
    Bitboard empty = ~occ;

    // Pawns
    {
        Bitboard pawns = pos.pieces(us, PAWN);
        int up = (us == WHITE) ? 8 : -8;
        Bitboard rank3 = (us == WHITE) ? Rank3BB : Rank6BB;

        // Single push
        Bitboard singlePush = (us == WHITE) ? (pawns << 8) & empty : (pawns >> 8) & empty;
        Bitboard doublePush = (us == WHITE) ? ((singlePush & rank3) << 8) & empty
                                            : ((singlePush & rank3) >> 8) & empty;

        while (singlePush) {
            Square to = BB::poplsb(singlePush);
            addPawnMoves(list, Square(to - up), to, false, us);
        }
        while (doublePush) {
            Square to = BB::poplsb(doublePush);
            list.add(Move::make(Square(to - 2 * up), to));
        }

        // Captures
        Bitboard leftAttacks, rightAttacks;
        if (us == WHITE) {
            leftAttacks  = (pawns & ~FileABB) << 7 & theirPieces;
            rightAttacks = (pawns & ~FileHBB) << 9 & theirPieces;
        } else {
            leftAttacks  = (pawns & ~FileHBB) >> 7 & theirPieces;
            rightAttacks = (pawns & ~FileABB) >> 9 & theirPieces;
        }
        while (leftAttacks) {
            Square to = BB::poplsb(leftAttacks);
            int delta = (us == WHITE) ? 7 : -7;
            addPawnMoves(list, Square(to - delta), to, true, us);
        }
        while (rightAttacks) {
            Square to = BB::poplsb(rightAttacks);
            int delta = (us == WHITE) ? 9 : -9;
            addPawnMoves(list, Square(to - delta), to, true, us);
        }

        // En passant
        Square ep = pos.epSquare();
        if (ep != NO_SQUARE) {
            Bitboard epAttackers = BB::PawnAttacks[them][ep] & pawns;
            while (epAttackers) {
                Square from = BB::poplsb(epAttackers);
                list.add(Move::make(from, ep, FLAG_ENPASSANT));
            }
        }
    }

    // Knights
    {
        Bitboard knights = pos.pieces(us, KNIGHT);
        while (knights) {
            Square from = BB::poplsb(knights);
            Bitboard targets = BB::KnightAttacks[from] & ~ourPieces;
            while (targets) {
                Square to = BB::poplsb(targets);
                list.add(Move::make(from, to));
            }
        }
    }

    // Bishops
    {
        Bitboard bishops = pos.pieces(us, BISHOP);
        while (bishops) {
            Square from = BB::poplsb(bishops);
            Bitboard targets = BB::bishopAttacks(from, occ) & ~ourPieces;
            while (targets) {
                Square to = BB::poplsb(targets);
                list.add(Move::make(from, to));
            }
        }
    }

    // Rooks
    {
        Bitboard rooks = pos.pieces(us, ROOK);
        while (rooks) {
            Square from = BB::poplsb(rooks);
            Bitboard targets = BB::rookAttacks(from, occ) & ~ourPieces;
            while (targets) {
                Square to = BB::poplsb(targets);
                list.add(Move::make(from, to));
            }
        }
    }

    // Queens
    {
        Bitboard queens = pos.pieces(us, QUEEN);
        while (queens) {
            Square from = BB::poplsb(queens);
            Bitboard targets = BB::queenAttacks(from, occ) & ~ourPieces;
            while (targets) {
                Square to = BB::poplsb(targets);
                list.add(Move::make(from, to));
            }
        }
    }

    // King
    {
        Square ksq = pos.kingSq(us);
        Bitboard targets = BB::KingAttacks[ksq] & ~ourPieces;
        while (targets) {
            Square to = BB::poplsb(targets);
            list.add(Move::make(ksq, to));
        }

        // Castling
        if (us == WHITE) {
            if ((pos.castlingRights() & WHITE_OO) &&
                !(occ & (squareBB(F1) | squareBB(G1))) &&
                !pos.isSquareAttacked(E1, them) &&
                !pos.isSquareAttacked(F1, them) &&
                !pos.isSquareAttacked(G1, them)) {
                list.add(Move::make(E1, G1, FLAG_CASTLING));
            }
            if ((pos.castlingRights() & WHITE_OOO) &&
                !(occ & (squareBB(B1) | squareBB(C1) | squareBB(D1))) &&
                !pos.isSquareAttacked(E1, them) &&
                !pos.isSquareAttacked(C1, them) &&
                !pos.isSquareAttacked(D1, them)) {
                list.add(Move::make(E1, C1, FLAG_CASTLING));
            }
        } else {
            if ((pos.castlingRights() & BLACK_OO) &&
                !(occ & (squareBB(F8) | squareBB(G8))) &&
                !pos.isSquareAttacked(E8, them) &&
                !pos.isSquareAttacked(F8, them) &&
                !pos.isSquareAttacked(G8, them)) {
                list.add(Move::make(E8, G8, FLAG_CASTLING));
            }
            if ((pos.castlingRights() & BLACK_OOO) &&
                !(occ & (squareBB(B8) | squareBB(C8) | squareBB(D8))) &&
                !pos.isSquareAttacked(E8, them) &&
                !pos.isSquareAttacked(C8, them) &&
                !pos.isSquareAttacked(D8, them)) {
                list.add(Move::make(E8, C8, FLAG_CASTLING));
            }
        }
    }
}

void generateLegal(Position& pos, MoveList& list) {
    MoveList pseudo;
    generatePseudoLegal(pos, pseudo);

    Color us = pos.sideToMove();

    for (int i = 0; i < pseudo.count; ++i) {
        StateInfo st;
        pos.makeMove(pseudo[i], st);
        // After makeMove, side has flipped, so check if our king (now ~side) is attacked
        if (!pos.isSquareAttacked(pos.kingSq(us), pos.sideToMove())) {
            list.add(pseudo[i]);
        }
        pos.unmakeMove(pseudo[i]);
    }
}

void generateCaptures(const Position& pos, MoveList& list) {
    Color us = pos.sideToMove();
    Color them = ~us;
    Bitboard ourPieces = pos.pieces(us);
    Bitboard theirPieces = pos.pieces(them);
    Bitboard occ = pos.allPieces();

    // Pawn captures + promotions
    {
        Bitboard pawns = pos.pieces(us, PAWN);
        Bitboard promoRank = (us == WHITE) ? Rank8BB : Rank1BB;

        // Captures
        Bitboard leftAttacks, rightAttacks;
        if (us == WHITE) {
            leftAttacks  = (pawns & ~FileABB) << 7 & theirPieces;
            rightAttacks = (pawns & ~FileHBB) << 9 & theirPieces;
        } else {
            leftAttacks  = (pawns & ~FileHBB) >> 7 & theirPieces;
            rightAttacks = (pawns & ~FileABB) >> 9 & theirPieces;
        }
        while (leftAttacks) {
            Square to = BB::poplsb(leftAttacks);
            int delta = (us == WHITE) ? 7 : -7;
            Square from = Square(to - delta);
            if (squareBB(to) & promoRank) {
                list.add(Move::make(from, to, FLAG_PROMOTION, QUEEN));
                list.add(Move::make(from, to, FLAG_PROMOTION, ROOK));
                list.add(Move::make(from, to, FLAG_PROMOTION, BISHOP));
                list.add(Move::make(from, to, FLAG_PROMOTION, KNIGHT));
            } else {
                list.add(Move::make(from, to));
            }
        }
        while (rightAttacks) {
            Square to = BB::poplsb(rightAttacks);
            int delta = (us == WHITE) ? 9 : -9;
            Square from = Square(to - delta);
            if (squareBB(to) & promoRank) {
                list.add(Move::make(from, to, FLAG_PROMOTION, QUEEN));
                list.add(Move::make(from, to, FLAG_PROMOTION, ROOK));
                list.add(Move::make(from, to, FLAG_PROMOTION, BISHOP));
                list.add(Move::make(from, to, FLAG_PROMOTION, KNIGHT));
            } else {
                list.add(Move::make(from, to));
            }
        }

        // Quiet queen promotions (pushes to 8th/1st rank)
        int up = (us == WHITE) ? 8 : -8;
        Bitboard pushPromos = ((us == WHITE) ? (pawns << 8) : (pawns >> 8)) & ~occ & promoRank;
        while (pushPromos) {
            Square to = BB::poplsb(pushPromos);
            list.add(Move::make(Square(to - up), to, FLAG_PROMOTION, QUEEN));
            list.add(Move::make(Square(to - up), to, FLAG_PROMOTION, ROOK));
            list.add(Move::make(Square(to - up), to, FLAG_PROMOTION, BISHOP));
            list.add(Move::make(Square(to - up), to, FLAG_PROMOTION, KNIGHT));
        }

        // En passant
        Square ep = pos.epSquare();
        if (ep != NO_SQUARE) {
            Bitboard epAttackers = BB::PawnAttacks[them][ep] & pawns;
            while (epAttackers) {
                Square from = BB::poplsb(epAttackers);
                list.add(Move::make(from, ep, FLAG_ENPASSANT));
            }
        }
    }

    // Knights
    {
        Bitboard knights = pos.pieces(us, KNIGHT);
        while (knights) {
            Square from = BB::poplsb(knights);
            Bitboard targets = BB::KnightAttacks[from] & theirPieces;
            while (targets) {
                list.add(Move::make(from, BB::poplsb(targets)));
            }
        }
    }

    // Bishops
    {
        Bitboard bishops = pos.pieces(us, BISHOP);
        while (bishops) {
            Square from = BB::poplsb(bishops);
            Bitboard targets = BB::bishopAttacks(from, occ) & theirPieces;
            while (targets) {
                list.add(Move::make(from, BB::poplsb(targets)));
            }
        }
    }

    // Rooks
    {
        Bitboard rooks = pos.pieces(us, ROOK);
        while (rooks) {
            Square from = BB::poplsb(rooks);
            Bitboard targets = BB::rookAttacks(from, occ) & theirPieces;
            while (targets) {
                list.add(Move::make(from, BB::poplsb(targets)));
            }
        }
    }

    // Queens
    {
        Bitboard queens = pos.pieces(us, QUEEN);
        while (queens) {
            Square from = BB::poplsb(queens);
            Bitboard targets = BB::queenAttacks(from, occ) & theirPieces;
            while (targets) {
                list.add(Move::make(from, BB::poplsb(targets)));
            }
        }
    }

    // King captures
    {
        Square ksq = pos.kingSq(us);
        Bitboard targets = BB::KingAttacks[ksq] & theirPieces;
        while (targets) {
            list.add(Move::make(ksq, BB::poplsb(targets)));
        }
    }

    (void)ourPieces;
}

} // namespace MoveGen
