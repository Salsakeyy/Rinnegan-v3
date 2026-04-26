#include "see.h"
#include "bitboard.h"

namespace SEE {

bool seeGE(const Position& pos, Move m, int threshold) {
    // Castling never captures and yields no material — by convention SEE = 0.
    if (m.flag() == FLAG_CASTLING) return threshold <= 0;

    Square from = m.from();
    Square to   = m.to();

    Piece movingPiece = pos.pieceOn(from);
    PieceType moverType = pieceType(movingPiece);

    // Initial gain: value of the captured piece (if any).
    Piece capturedPiece = pos.pieceOn(to);
    int swap = (capturedPiece != NO_PIECE) ? PieceValue[pieceType(capturedPiece)] : 0;

    // En passant: the captured pawn lives on a different square (same file as
    // `to`, same rank as `from`).
    Bitboard epExtra = 0;
    if (m.flag() == FLAG_ENPASSANT) {
        swap = PieceValue[PAWN];
        Square capSq = makeSquare(fileOf(to), rankOf(from));
        epExtra = squareBB(capSq);
    }

    // Promotion: pawn becomes the promo piece, so net gain includes (promo - pawn)
    // and the attacker's value from this point on is the promo piece.
    PieceType attackerType = moverType;
    if (m.flag() == FLAG_PROMOTION) {
        attackerType = m.promoPiece();
        swap += PieceValue[attackerType] - PieceValue[PAWN];
    }

    swap -= threshold;
    if (swap < 0) return false;            // Even if our piece is free, can't reach threshold.

    swap = PieceValue[attackerType] - swap;
    if (swap <= 0) return true;            // Even if we lose our piece, threshold already met.

    // Set up swap simulation.
    Bitboard occ = pos.allPieces() ^ squareBB(from) ^ squareBB(to);
    occ ^= epExtra;

    Bitboard attackers = pos.attackersTo(to, occ) & occ;
    Color stm = ~pos.sideToMove();         // Opponent moves next.

    int res = 1;                           // 1 = mover wins so far, 0 = mover loses.
    Bitboard bb;

    while (true) {
        attackers &= occ;
        Bitboard stmAttackers = attackers & pos.pieces(stm);
        if (!stmAttackers) break;

        res ^= 1;

        if ((bb = stmAttackers & pos.pieces(stm, PAWN))) {
            if ((swap = PieceValue[PAWN] - swap) < res) break;
            occ ^= (bb & (~bb + 1));       // pop lowest bit
            attackers |= BB::bishopAttacks(to, occ) & (pos.pieces(BISHOP) | pos.pieces(QUEEN));
        }
        else if ((bb = stmAttackers & pos.pieces(stm, KNIGHT))) {
            if ((swap = PieceValue[KNIGHT] - swap) < res) break;
            occ ^= (bb & (~bb + 1));
        }
        else if ((bb = stmAttackers & pos.pieces(stm, BISHOP))) {
            if ((swap = PieceValue[BISHOP] - swap) < res) break;
            occ ^= (bb & (~bb + 1));
            attackers |= BB::bishopAttacks(to, occ) & (pos.pieces(BISHOP) | pos.pieces(QUEEN));
        }
        else if ((bb = stmAttackers & pos.pieces(stm, ROOK))) {
            if ((swap = PieceValue[ROOK] - swap) < res) break;
            occ ^= (bb & (~bb + 1));
            attackers |= BB::rookAttacks(to, occ) & (pos.pieces(ROOK) | pos.pieces(QUEEN));
        }
        else if ((bb = stmAttackers & pos.pieces(stm, QUEEN))) {
            if ((swap = PieceValue[QUEEN] - swap) < res) break;
            occ ^= (bb & (~bb + 1));
            attackers |= (BB::bishopAttacks(to, occ) & (pos.pieces(BISHOP) | pos.pieces(QUEEN)))
                       | (BB::rookAttacks(to, occ)   & (pos.pieces(ROOK)   | pos.pieces(QUEEN)));
        }
        else {
            // King: capturing into a still-defended square is illegal, so we lose.
            return (attackers & ~pos.pieces(stm)) ? bool(res ^ 1) : bool(res);
        }

        stm = ~stm;
    }

    return bool(res);
}

} // namespace SEE
