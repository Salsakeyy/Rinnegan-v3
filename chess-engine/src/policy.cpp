#include "policy.h"
#include "bitboard.h"
#include "see.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define POLICY_HAS_NEON 1
#endif

namespace Policy {
namespace {

constexpr char MAGIC[8] = {'R', 'I', 'N', 'P', 'O', 'L', '1', '\0'};

// Latency counters. Defined here so both v1 and v2 scoring paths can write
// to them without forward references. Read via Policy::readPerfCounters().
struct AtomicPerf {
    std::atomic<uint64_t> featureNanos{0};
    std::atomic<uint64_t> forwardNanos{0};
    std::atomic<uint64_t> calls{0};
};
AtomicPerf g_perf;

constexpr int MIN_FEATURE_DIM = 17;
constexpr int NEW32_FEATURE_DIM = 32;
constexpr int NEW64_FEATURE_DIM = 64;
constexpr int MAX_FEATURE_DIM = NEW64_FEATURE_DIM;
constexpr int MAX_HIDDEN = 1024;

enum class FeatureLayout : uint32_t {
    Legacy = 0,
    New32 = 1,
    New64 = 2,
};

struct PositionFeatures {
    Color us;
    float pieceCount;
    float matBalance;
    float fullmovePhase;
    int enemyKingFile = 0;
    int enemyKingRank = 0;
    bool hasEnemyKing = false;
    bool wasInCheck;
};

// Per-from-square cache for `attackedFrom` (whether opponent attacks the
// origin square in the unmodified root position). Only 8-15 unique from
// squares typically appear in a root move list, so caching saves ~70% of
// the isSquareAttacked calls compared to per-move recomputation.
struct AttackCache {
    int8_t attackedFrom[64];
    void reset() { std::memset(attackedFrom, -1, sizeof(attackedFrom)); }
};

struct Model {
    int input = 0;
    int hidden1 = 0;
    int hidden2 = 0;
    FeatureLayout layout = FeatureLayout::Legacy;
    std::vector<float> w1;
    std::vector<float> b1;
    std::vector<float> w2;
    std::vector<float> b2;
    std::vector<float> w3;
    float b3 = 0.0f;
    std::string path;
};

Model model;

int pieceValue(PieceType pt) {
    switch (pt) {
    case PAWN:   return 100;
    case KNIGHT: return 320;
    case BISHOP: return 330;
    case ROOK:   return 500;
    case QUEEN:  return 900;
    default:     return 0;
    }
}

float materialBalanceForSide(const Position& pos) {
    int white = 0;
    int black = 0;
    for (int sq = 0; sq < 64; ++sq) {
        Piece piece = pos.pieceOn(Square(sq));
        if (piece == NO_PIECE)
            continue;
        int value = pieceValue(pieceType(piece));
        if (pieceColor(piece) == WHITE)
            white += value;
        else
            black += value;
    }

    int balance = white - black;
    if (pos.sideToMove() == BLACK)
        balance = -balance;
    return std::clamp(float(balance) / 2000.0f, -1.0f, 1.0f);
}

float centrality(int file, int rank) {
    return (3.5f - std::max(std::abs(float(file) - 3.5f), std::abs(float(rank) - 3.5f))) / 3.5f;
}

// Single-pass SEE value via bracket-and-bisect on log-spaced thresholds. The
// trained features only consume sign(see), see>=0/<0, and clamp(see/1000, ±2),
// so 100cp resolution is sufficient. ~7 seeGE calls in the worst case (vs 12
// for the prior [-2000,2000]/1cp binary search), and typically far fewer
// because most legal root moves are quiet (SEE = 0) or simple captures with
// extreme SEE that the early-out resolves in one or two probes.
int staticExchangeScore(const Position& pos, Move move) {
    if (SEE::seeGE(pos, move, 0)) {
        // SEE >= 0: probe upward to localize.
        if (SEE::seeGE(pos, move, 100)) {
            if (SEE::seeGE(pos, move, 500)) {
                if (SEE::seeGE(pos, move, 1000)) return 1000;
                if (SEE::seeGE(pos, move, 700)) return 700;
                return 500;
            }
            if (SEE::seeGE(pos, move, 300)) return 300;
            return 100;
        }
        return 0;
    }
    // SEE < 0: probe downward.
    if (SEE::seeGE(pos, move, -100)) return -50;
    if (SEE::seeGE(pos, move, -300)) return -100;
    if (SEE::seeGE(pos, move, -500)) return -300;
    if (SEE::seeGE(pos, move, -1000)) return -500;
    return -1000;
}

PositionFeatures computePositionFeatures(const Position& pos) {
    PositionFeatures pf;
    pf.us = pos.sideToMove();
    int pieces = BB::popcount(pos.allPieces());
    pf.pieceCount = float(pieces) / 32.0f;
    pf.matBalance = materialBalanceForSide(pos);
    pf.fullmovePhase = std::log2(float(std::max(1, pos.fullmoveNumber))) / 10.0f;
    Square enemyKing = pos.kingSq(~pf.us);
    pf.hasEnemyKing = enemyKing != NO_SQUARE;
    if (pf.hasEnemyKing) {
        pf.enemyKingFile = fileOf(enemyKing);
        pf.enemyKingRank = rankOf(enemyKing);
        if (pf.us == BLACK)
            pf.enemyKingRank = 7 - pf.enemyKingRank;
    }
    pf.wasInCheck = pos.inCheck();
    return pf;
}

Bitboard attacksFromPiece(const Position& pos, Square square, Piece piece) {
    if (piece == NO_PIECE || square == NO_SQUARE)
        return 0;
    Bitboard occ = pos.allPieces();
    switch (pieceType(piece)) {
    case PAWN:   return BB::PawnAttacks[pieceColor(piece)][square];
    case KNIGHT: return BB::KnightAttacks[square];
    case BISHOP: return BB::bishopAttacks(square, occ);
    case ROOK:   return BB::rookAttacks(square, occ);
    case QUEEN:  return BB::queenAttacks(square, occ);
    case KING:   return BB::KingAttacks[square];
    default:     return 0;
    }
}

int mobilityCount(const Position& pos, Square square) {
    Piece piece = pos.pieceOn(square);
    return piece == NO_PIECE ? 0 : BB::popcount(attacksFromPiece(pos, square, piece));
}

int lowestAttackerValue(const Position& pos, Color color, Square square) {
    Bitboard attackers = pos.attackersTo(square, pos.allPieces()) & pos.pieces(color);
    int best = 1000000;
    while (attackers) {
        Square attacker = BB::poplsb(attackers);
        Piece piece = pos.pieceOn(attacker);
        if (piece != NO_PIECE)
            best = std::min(best, pieceValue(pieceType(piece)));
    }
    return best == 1000000 ? 0 : best;
}

int attackerCount(const Position& pos, Color color, Square square) {
    return BB::popcount(pos.attackersTo(square, pos.allPieces()) & pos.pieces(color));
}

bool isPassedPawnAfterMove(const Position& pos, Color us, Square square) {
    int file = fileOf(square);
    int rank = rankOf(square);
    Color them = ~us;
    for (int df = -1; df <= 1; ++df) {
        int f = file + df;
        if (f < 0 || f > 7)
            continue;
        if (us == WHITE) {
            for (int r = rank + 1; r <= 7; ++r) {
                if (pos.pieceOn(makeSquare(f, r)) == makePiece(them, PAWN))
                    return false;
            }
        } else {
            for (int r = rank - 1; r >= 0; --r) {
                if (pos.pieceOn(makeSquare(f, r)) == makePiece(them, PAWN))
                    return false;
            }
        }
    }
    return true;
}

int kingZoneAttackCount(const Position& pos, Square from, Piece piece, Square enemyKing) {
    if (piece == NO_PIECE || enemyKing == NO_SQUARE)
        return 0;
    Bitboard attacks = attacksFromPiece(pos, from, piece);
    int count = 0;
    int kf = fileOf(enemyKing);
    int kr = rankOf(enemyKing);
    while (attacks) {
        Square target = BB::poplsb(attacks);
        if (std::max(std::abs(fileOf(target) - kf), std::abs(rankOf(target) - kr)) <= 1)
            ++count;
    }
    return count;
}

bool isRookOpenFile(const Position& pos, int file) {
    return (pos.pieces(PAWN) & fileBB(file)) == 0;
}

bool isRookSemiOpenFile(const Position& pos, Color us, int file) {
    return (pos.pieces(us, PAWN) & fileBB(file)) == 0 &&
           (pos.pieces(~us, PAWN) & fileBB(file)) != 0;
}

void setOneHotPiece(PieceType pt, const PieceType* values, int count, float* out) {
    for (int i = 0; i < count; ++i)
        out[i] = (pt == values[i]) ? 1.0f : 0.0f;
}

void fillFeaturesWithCtx(Position& pos, Move move, const PositionFeatures& pf,
                         AttackCache& ac, float* out, int featureDim) {
    Piece mover = pos.pieceOn(move.from());
    Piece captured = (move.flag() == FLAG_ENPASSANT) ? makePiece(~pf.us, PAWN) : pos.pieceOn(move.to());

    int fromFile = fileOf(move.from());
    int fromRank = rankOf(move.from());
    int toFile = fileOf(move.to());
    int toRank = rankOf(move.to());
    if (pf.us == BLACK) {
        fromRank = 7 - fromRank;
        toRank = 7 - toRank;
    }

    bool isCapture = captured != NO_PIECE;
    bool isPromotion = move.flag() == FLAG_PROMOTION;
    int moverValue = mover == NO_PIECE ? 0 : pieceValue(pieceType(mover));
    int capturedValue = captured == NO_PIECE ? 0 : pieceValue(pieceType(captured));

    // Single makeMove/unmakeMove pair: extract `gives_check` (always needed)
    // and `attackedTo` (only for 32-feat) in one go.
    StateInfo state;
    pos.makeMove(move, state);
    bool givesCheck = pos.inCheck();
    bool attackedTo = (featureDim == NEW32_FEATURE_DIM)
        ? pos.isSquareAttacked(move.to(), pos.sideToMove())
        : false;
    pos.unmakeMove(move);

    out[0] = float(fromFile) / 7.0f;
    out[1] = float(fromRank) / 7.0f;
    out[2] = float(toFile) / 7.0f;
    out[3] = float(toRank) / 7.0f;
    out[4] = float(toFile - fromFile) / 7.0f;
    out[5] = float(toRank - fromRank) / 7.0f;
    out[6] = mover == NO_PIECE ? 0.0f : float(int(pieceType(mover)) + 1) / 6.0f;
    out[7] = captured == NO_PIECE ? 0.0f : float(int(pieceType(captured)) + 1) / 6.0f;
    out[8] = isPromotion ? float(int(move.promoPiece()) + 1) / 6.0f : 0.0f;
    out[9] = isCapture ? 1.0f : 0.0f;
    out[10] = isPromotion ? 1.0f : 0.0f;
    out[11] = move.flag() == FLAG_CASTLING ? 1.0f : 0.0f;
    out[12] = move.flag() == FLAG_ENPASSANT ? 1.0f : 0.0f;
    out[13] = givesCheck ? 1.0f : 0.0f;
    out[14] = pf.pieceCount;
    out[15] = pf.matBalance;
    out[16] = pf.fullmovePhase;

    if (featureDim == MIN_FEATURE_DIM)
        return;

    float fromCentrality = centrality(fromFile, fromRank);
    float toCentrality = centrality(toFile, toRank);
    // SEE for a quiet move = "what we lose if the opponent recaptures the
    // landing square", i.e. attackedTo determines the outcome. Cheap shortcut:
    // when nothing attacks `to`, SEE is 0; otherwise fall back to the swap
    // simulator. Avoids the swap calls on the majority of root quiets.
    int see;
    if (isCapture || isPromotion || attackedTo)
        see = staticExchangeScore(pos, move);
    else
        see = 0;

    int8_t cached = ac.attackedFrom[move.from()];
    bool attackedFrom;
    if (cached < 0) {
        attackedFrom = pos.isSquareAttacked(move.from(), ~pf.us);
        ac.attackedFrom[move.from()] = attackedFrom ? 1 : 0;
    } else {
        attackedFrom = bool(cached);
    }

    float advancement = 0.0f;
    if (mover != NO_PIECE && pieceType(mover) == PAWN)
        advancement = float(toRank) / 7.0f;

    out[17] = pf.us == WHITE ? 1.0f : 0.0f;
    out[18] = fromCentrality;
    out[19] = toCentrality;
    out[20] = toCentrality - fromCentrality;
    out[21] = float(moverValue) / 900.0f;
    out[22] = float(capturedValue) / 900.0f;
    out[23] = std::clamp(float(see) / 1000.0f, -2.0f, 2.0f);
    out[24] = see >= 0 ? 1.0f : 0.0f;
    out[25] = see < 0 ? 1.0f : 0.0f;
    out[26] = (mover != NO_PIECE && pieceType(mover) == PAWN && std::abs(toRank - fromRank) == 2) ? 1.0f : 0.0f;
    out[27] = attackedFrom ? 1.0f : 0.0f;
    out[28] = attackedTo ? 1.0f : 0.0f;
    out[29] = pf.wasInCheck ? 1.0f : 0.0f;
    out[30] = advancement;
    out[31] = float(std::abs(toFile - fromFile) + std::abs(toRank - fromRank)) / 14.0f;
}

void fillNew32FeaturesWithCtx(Position& pos, Move move, const PositionFeatures& pf,
                              AttackCache& ac, float* out) {
    std::fill_n(out, NEW32_FEATURE_DIM, 0.0f);

    Piece mover = pos.pieceOn(move.from());
    Piece captured = (move.flag() == FLAG_ENPASSANT) ? makePiece(~pf.us, PAWN) : pos.pieceOn(move.to());
    PieceType moverType = mover == NO_PIECE ? NO_PIECE_TYPE : pieceType(mover);
    PieceType capturedType = captured == NO_PIECE ? NO_PIECE_TYPE : pieceType(captured);

    int fromFile = fileOf(move.from());
    int fromRank = rankOf(move.from());
    int toFile = fileOf(move.to());
    int toRank = rankOf(move.to());
    if (pf.us == BLACK) {
        fromRank = 7 - fromRank;
        toRank = 7 - toRank;
    }

    float kingDistance = 1.0f;
    float inKingRing = 0.0f;
    if (pf.hasEnemyKing) {
        int distance = std::max(std::abs(toFile - pf.enemyKingFile),
                                std::abs(toRank - pf.enemyKingRank));
        kingDistance = float(distance) / 7.0f;
        inKingRing = distance <= 1 ? 1.0f : 0.0f;
    }

    int beforeMobility = mobilityCount(pos, move.from());
    bool attacksMoreValuable = false;
    bool attacksUndefended = false;

    StateInfo state;
    pos.makeMove(move, state);
    bool givesCheck = pos.inCheck();
    Color themAfter = pos.sideToMove();
    Color usAfter = ~themAfter;
    bool attackedTo = pos.isSquareAttacked(move.to(), themAfter);
    int enemyAttacker = lowestAttackerValue(pos, themAfter, move.to());
    int ownDefender = lowestAttackerValue(pos, usAfter, move.to());

    Piece movedPiece = pos.pieceOn(move.to());
    int moverValue = movedPiece == NO_PIECE ? 0 : pieceValue(pieceType(movedPiece));
    int afterMobility = mobilityCount(pos, move.to());
    Bitboard attacks = attacksFromPiece(pos, move.to(), movedPiece);
    Bitboard occ = pos.allPieces();
    while (attacks) {
        Square target = BB::poplsb(attacks);
        Piece victim = pos.pieceOn(target);
        if (victim == NO_PIECE || pieceColor(victim) != themAfter || pieceType(victim) == KING)
            continue;

        int victimValue = pieceValue(pieceType(victim));
        if (victimValue > moverValue)
            attacksMoreValuable = true;

        Bitboard defenders = pos.attackersTo(target, occ) & pos.pieces(themAfter);
        if (!defenders)
            attacksUndefended = true;
    }
    pos.unmakeMove(move);

    bool isCapture = captured != NO_PIECE;
    bool isPromotion = move.flag() == FLAG_PROMOTION;
    int see = (isCapture || isPromotion || attackedTo) ? staticExchangeScore(pos, move) : 0;

    int8_t cached = ac.attackedFrom[move.from()];
    bool attackedFrom;
    if (cached < 0) {
        attackedFrom = pos.isSquareAttacked(move.from(), ~pf.us);
        ac.attackedFrom[move.from()] = attackedFrom ? 1 : 0;
    } else {
        attackedFrom = bool(cached);
    }

    float mobilityDelta = std::clamp(float(afterMobility - beforeMobility) / 28.0f, -1.0f, 1.0f);

    static constexpr PieceType moverTypes[6] = {PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING};
    static constexpr PieceType capturedTypes[5] = {PAWN, KNIGHT, BISHOP, ROOK, QUEEN};

    out[0] = float(fromFile) / 7.0f;
    out[1] = float(fromRank) / 7.0f;
    out[2] = float(toFile) / 7.0f;
    out[3] = float(toRank) / 7.0f;
    out[4] = float(toRank - fromRank) / 7.0f;
    setOneHotPiece(moverType, moverTypes, 6, out + 5);
    setOneHotPiece(capturedType, capturedTypes, 5, out + 11);
    out[16] = isPromotion ? float(int(move.promoPiece()) + 1) / 6.0f : 0.0f;
    out[17] = isCapture ? 1.0f : 0.0f;
    out[18] = isPromotion ? 1.0f : 0.0f;
    out[19] = move.flag() == FLAG_CASTLING ? 1.0f : 0.0f;
    out[20] = givesCheck ? 1.0f : 0.0f;
    out[21] = pf.matBalance;
    out[22] = pf.pieceCount;
    out[23] = std::clamp(float(see) / 1000.0f, -2.0f, 2.0f);
    out[24] = attackedFrom ? 1.0f : 0.0f;
    out[25] = float(enemyAttacker) / 900.0f;
    out[26] = float(ownDefender) / 900.0f;
    out[27] = kingDistance;
    out[28] = inKingRing;
    out[29] = attacksMoreValuable ? 1.0f : 0.0f;
    out[30] = attacksUndefended ? 1.0f : 0.0f;
    out[31] = mobilityDelta;
}

void fillNew64FeaturesWithCtx(Position& pos, Move move, const PositionFeatures& pf,
                              AttackCache& ac, float* out) {
    std::fill_n(out, NEW64_FEATURE_DIM, 0.0f);
    fillNew32FeaturesWithCtx(pos, move, pf, ac, out);

    Color us = pf.us;
    Piece mover = pos.pieceOn(move.from());
    Piece captured = (move.flag() == FLAG_ENPASSANT) ? makePiece(~us, PAWN) : pos.pieceOn(move.to());

    int fromFile = fileOf(move.from());
    int fromRank = rankOf(move.from());
    int toFile = fileOf(move.to());
    int toRank = rankOf(move.to());
    if (us == BLACK) {
        fromRank = 7 - fromRank;
        toRank = 7 - toRank;
    }

    float fromCentrality = centrality(fromFile, fromRank);
    float toCentrality = centrality(toFile, toRank);
    int moverValue = mover == NO_PIECE ? 0 : pieceValue(pieceType(mover));
    int capturedValue = captured == NO_PIECE ? 0 : pieceValue(pieceType(captured));

    StateInfo state;
    pos.makeMove(move, state);
    bool givesCheck = pos.inCheck();
    Color themAfter = pos.sideToMove();
    Color usAfter = ~themAfter;
    bool attackedTo = pos.isSquareAttacked(move.to(), themAfter);
    int ownDefenders = attackerCount(pos, usAfter, move.to());
    int enemyAttackers = attackerCount(pos, themAfter, move.to());
    int ownDefender = lowestAttackerValue(pos, usAfter, move.to());
    int enemyAttacker = lowestAttackerValue(pos, themAfter, move.to());
    Piece movedPiece = pos.pieceOn(move.to());
    int movedValue = movedPiece == NO_PIECE ? moverValue : pieceValue(pieceType(movedPiece));
    Square enemyKing = pos.kingSq(themAfter);
    int kingZoneAttacks = kingZoneAttackCount(pos, move.to(), movedPiece, enemyKing);
    bool passedPawn = movedPiece != NO_PIECE && pieceType(movedPiece) == PAWN &&
                      isPassedPawnAfterMove(pos, usAfter, move.to());
    bool rookOpen = movedPiece != NO_PIECE && pieceType(movedPiece) == ROOK &&
                    isRookOpenFile(pos, fileOf(move.to()));
    bool rookSemiOpen = movedPiece != NO_PIECE && pieceType(movedPiece) == ROOK &&
                        isRookSemiOpenFile(pos, usAfter, fileOf(move.to()));
    pos.unmakeMove(move);

    bool capture = captured != NO_PIECE;
    bool promotion = move.flag() == FLAG_PROMOTION;
    int see = (capture || promotion || attackedTo) ? staticExchangeScore(pos, move) : 0;
    bool hangingAfter = attackedTo && ownDefenders == 0;
    bool enPriseAfter = enemyAttacker > 0 && enemyAttacker < movedValue;
    float pawnAdvance = (mover != NO_PIECE && pieceType(mover) == PAWN) ? float(toRank) / 7.0f : 0.0f;
    float promotionDistance = (mover != NO_PIECE && pieceType(mover) == PAWN)
        ? 1.0f - (float(7 - toRank) / 6.0f)
        : 0.0f;
    bool doublePawnPush = mover != NO_PIECE && pieceType(mover) == PAWN &&
                          std::abs(toRank - fromRank) == 2;
    bool developmentMove = mover != NO_PIECE &&
        (pieceType(mover) == KNIGHT || pieceType(mover) == BISHOP) &&
        fromRank == 0 && toRank > fromRank;

    out[32] = fromCentrality;
    out[33] = toCentrality;
    out[34] = toCentrality - fromCentrality;
    out[35] = float(toFile - fromFile) / 7.0f;
    out[36] = float(std::abs(toFile - fromFile)) / 7.0f;
    out[37] = float(std::abs(toFile - fromFile) + std::abs(toRank - fromRank)) / 14.0f;
    out[38] = float(moverValue) / 900.0f;
    out[39] = float(capturedValue) / 900.0f;
    out[40] = us == WHITE ? 1.0f : 0.0f;
    out[41] = pf.wasInCheck ? 1.0f : 0.0f;
    out[42] = attackedTo ? 1.0f : 0.0f;
    out[43] = see >= 0 ? 1.0f : 0.0f;
    out[44] = see < 0 ? 1.0f : 0.0f;
    out[45] = float(ownDefenders) / 8.0f;
    out[46] = float(enemyAttackers) / 8.0f;
    out[47] = float(ownDefender) / 900.0f;
    out[48] = float(enemyAttacker) / 900.0f;
    out[49] = hangingAfter ? 1.0f : 0.0f;
    out[50] = enPriseAfter ? 1.0f : 0.0f;
    out[51] = ownDefenders > 0 ? 1.0f : 0.0f;
    out[52] = givesCheck ? 1.0f : 0.0f;
    out[53] = out[29];
    out[54] = out[30];
    out[55] = float(kingZoneAttacks) / 8.0f;
    out[56] = out[28];
    out[57] = passedPawn ? 1.0f : 0.0f;
    out[58] = pawnAdvance;
    out[59] = std::clamp(promotionDistance, 0.0f, 1.0f);
    out[60] = doublePawnPush ? 1.0f : 0.0f;
    out[61] = developmentMove ? 1.0f : 0.0f;
    out[62] = rookOpen ? 1.0f : 0.0f;
    out[63] = rookSemiOpen ? 1.0f : 0.0f;
}

// ---------- MLP forward kernels (no feature change) ----------
//
// Replace the original per-row scalar `linearRelu` with two vectorized
// kernels that produce bit-equivalent results modulo float associativity.
//
//   layerForward      : out[H] = ReLU(b[H] + W[H,F] @ in[F])
//   denseDotPlusBias  :  scalar = b + w[F] . in[F]   (final layer)
//
// Both work for any F, H. The AVX2 path keeps 8 row-accumulators in
// flight to amortize input loads over multiple weight rows. Weight
// matrices come from std::vector<float> (4-byte aligned), so we use
// loadu_ps for them; `in` and `out` callers pass alignas(32) buffers.

#if defined(__AVX2__)

static inline float hsum256_ps(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    __m128 s  = _mm_add_ps(lo, hi);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    return _mm_cvtss_f32(s);
}

void layerForward(const float* W, const float* b, const float* in,
                  int F, int H, float* out) {
    int r = 0;
    const int F8 = F & ~7;
    for (; r + 8 <= H; r += 8) {
        __m256 a0 = _mm256_setzero_ps();
        __m256 a1 = _mm256_setzero_ps();
        __m256 a2 = _mm256_setzero_ps();
        __m256 a3 = _mm256_setzero_ps();
        __m256 a4 = _mm256_setzero_ps();
        __m256 a5 = _mm256_setzero_ps();
        __m256 a6 = _mm256_setzero_ps();
        __m256 a7 = _mm256_setzero_ps();
        const float* p0 = W + (r + 0) * F;
        const float* p1 = W + (r + 1) * F;
        const float* p2 = W + (r + 2) * F;
        const float* p3 = W + (r + 3) * F;
        const float* p4 = W + (r + 4) * F;
        const float* p5 = W + (r + 5) * F;
        const float* p6 = W + (r + 6) * F;
        const float* p7 = W + (r + 7) * F;
        int i = 0;
        for (; i < F8; i += 8) {
            __m256 x = _mm256_load_ps(in + i);
            a0 = _mm256_fmadd_ps(_mm256_loadu_ps(p0 + i), x, a0);
            a1 = _mm256_fmadd_ps(_mm256_loadu_ps(p1 + i), x, a1);
            a2 = _mm256_fmadd_ps(_mm256_loadu_ps(p2 + i), x, a2);
            a3 = _mm256_fmadd_ps(_mm256_loadu_ps(p3 + i), x, a3);
            a4 = _mm256_fmadd_ps(_mm256_loadu_ps(p4 + i), x, a4);
            a5 = _mm256_fmadd_ps(_mm256_loadu_ps(p5 + i), x, a5);
            a6 = _mm256_fmadd_ps(_mm256_loadu_ps(p6 + i), x, a6);
            a7 = _mm256_fmadd_ps(_mm256_loadu_ps(p7 + i), x, a7);
        }
        float s0 = hsum256_ps(a0) + b[r + 0];
        float s1 = hsum256_ps(a1) + b[r + 1];
        float s2 = hsum256_ps(a2) + b[r + 2];
        float s3 = hsum256_ps(a3) + b[r + 3];
        float s4 = hsum256_ps(a4) + b[r + 4];
        float s5 = hsum256_ps(a5) + b[r + 5];
        float s6 = hsum256_ps(a6) + b[r + 6];
        float s7 = hsum256_ps(a7) + b[r + 7];
        for (; i < F; ++i) {
            float x = in[i];
            s0 += p0[i] * x; s1 += p1[i] * x;
            s2 += p2[i] * x; s3 += p3[i] * x;
            s4 += p4[i] * x; s5 += p5[i] * x;
            s6 += p6[i] * x; s7 += p7[i] * x;
        }
        out[r + 0] = s0 > 0.0f ? s0 : 0.0f;
        out[r + 1] = s1 > 0.0f ? s1 : 0.0f;
        out[r + 2] = s2 > 0.0f ? s2 : 0.0f;
        out[r + 3] = s3 > 0.0f ? s3 : 0.0f;
        out[r + 4] = s4 > 0.0f ? s4 : 0.0f;
        out[r + 5] = s5 > 0.0f ? s5 : 0.0f;
        out[r + 6] = s6 > 0.0f ? s6 : 0.0f;
        out[r + 7] = s7 > 0.0f ? s7 : 0.0f;
    }
    // Tail rows.
    for (; r < H; ++r) {
        const float* p = W + r * F;
        __m256 acc = _mm256_setzero_ps();
        int i = 0;
        for (; i < F8; i += 8) {
            __m256 x = _mm256_load_ps(in + i);
            acc = _mm256_fmadd_ps(_mm256_loadu_ps(p + i), x, acc);
        }
        float sum = hsum256_ps(acc) + b[r];
        for (; i < F; ++i) sum += p[i] * in[i];
        out[r] = sum > 0.0f ? sum : 0.0f;
    }
}

float denseDotPlusBias(const float* w, float bias, const float* in, int F) {
    const int F8 = F & ~7;
    __m256 acc = _mm256_setzero_ps();
    int i = 0;
    for (; i < F8; i += 8) {
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(w + i), _mm256_load_ps(in + i), acc);
    }
    float sum = hsum256_ps(acc) + bias;
    for (; i < F; ++i) sum += w[i] * in[i];
    return sum;
}

#elif defined(POLICY_HAS_NEON)

// 4-wide NEON kernel. Apple Silicon has 4 FP NEON pipes with 1-cycle
// fmla throughput each, so keeping 8 row-accumulators in flight gives
// the scheduler enough independent dependency chains to saturate.

void layerForward(const float* W, const float* b, const float* in,
                  int F, int H, float* out) {
    int r = 0;
    const int F4 = F & ~3;
    for (; r + 8 <= H; r += 8) {
        float32x4_t a0 = vdupq_n_f32(0.0f), a1 = a0, a2 = a0, a3 = a0;
        float32x4_t a4 = vdupq_n_f32(0.0f), a5 = a4, a6 = a4, a7 = a4;
        const float* p0 = W + (r + 0) * F;
        const float* p1 = W + (r + 1) * F;
        const float* p2 = W + (r + 2) * F;
        const float* p3 = W + (r + 3) * F;
        const float* p4 = W + (r + 4) * F;
        const float* p5 = W + (r + 5) * F;
        const float* p6 = W + (r + 6) * F;
        const float* p7 = W + (r + 7) * F;
        int i = 0;
        for (; i < F4; i += 4) {
            float32x4_t x = vld1q_f32(in + i);
            a0 = vfmaq_f32(a0, vld1q_f32(p0 + i), x);
            a1 = vfmaq_f32(a1, vld1q_f32(p1 + i), x);
            a2 = vfmaq_f32(a2, vld1q_f32(p2 + i), x);
            a3 = vfmaq_f32(a3, vld1q_f32(p3 + i), x);
            a4 = vfmaq_f32(a4, vld1q_f32(p4 + i), x);
            a5 = vfmaq_f32(a5, vld1q_f32(p5 + i), x);
            a6 = vfmaq_f32(a6, vld1q_f32(p6 + i), x);
            a7 = vfmaq_f32(a7, vld1q_f32(p7 + i), x);
        }
        float s0 = vaddvq_f32(a0) + b[r + 0];
        float s1 = vaddvq_f32(a1) + b[r + 1];
        float s2 = vaddvq_f32(a2) + b[r + 2];
        float s3 = vaddvq_f32(a3) + b[r + 3];
        float s4 = vaddvq_f32(a4) + b[r + 4];
        float s5 = vaddvq_f32(a5) + b[r + 5];
        float s6 = vaddvq_f32(a6) + b[r + 6];
        float s7 = vaddvq_f32(a7) + b[r + 7];
        for (; i < F; ++i) {
            float x = in[i];
            s0 += p0[i] * x; s1 += p1[i] * x;
            s2 += p2[i] * x; s3 += p3[i] * x;
            s4 += p4[i] * x; s5 += p5[i] * x;
            s6 += p6[i] * x; s7 += p7[i] * x;
        }
        out[r + 0] = s0 > 0.0f ? s0 : 0.0f;
        out[r + 1] = s1 > 0.0f ? s1 : 0.0f;
        out[r + 2] = s2 > 0.0f ? s2 : 0.0f;
        out[r + 3] = s3 > 0.0f ? s3 : 0.0f;
        out[r + 4] = s4 > 0.0f ? s4 : 0.0f;
        out[r + 5] = s5 > 0.0f ? s5 : 0.0f;
        out[r + 6] = s6 > 0.0f ? s6 : 0.0f;
        out[r + 7] = s7 > 0.0f ? s7 : 0.0f;
    }
    for (; r < H; ++r) {
        const float* p = W + r * F;
        float32x4_t acc = vdupq_n_f32(0.0f);
        int i = 0;
        for (; i < F4; i += 4) {
            acc = vfmaq_f32(acc, vld1q_f32(p + i), vld1q_f32(in + i));
        }
        float sum = vaddvq_f32(acc) + b[r];
        for (; i < F; ++i) sum += p[i] * in[i];
        out[r] = sum > 0.0f ? sum : 0.0f;
    }
}

float denseDotPlusBias(const float* w, float bias, const float* in, int F) {
    const int F4 = F & ~3;
    float32x4_t acc = vdupq_n_f32(0.0f);
    int i = 0;
    for (; i < F4; i += 4) {
        acc = vfmaq_f32(acc, vld1q_f32(w + i), vld1q_f32(in + i));
    }
    float sum = vaddvq_f32(acc) + bias;
    for (; i < F; ++i) sum += w[i] * in[i];
    return sum;
}

#else  // scalar fallback (no AVX2, no NEON)

void layerForward(const float* W, const float* b, const float* in,
                  int F, int H, float* out) {
    for (int r = 0; r < H; ++r) {
        const float* p = W + r * F;
        float sum = b[r];
        for (int i = 0; i < F; ++i) sum += p[i] * in[i];
        out[r] = sum > 0.0f ? sum : 0.0f;
    }
}

float denseDotPlusBias(const float* w, float bias, const float* in, int F) {
    float sum = bias;
    for (int i = 0; i < F; ++i) sum += w[i] * in[i];
    return sum;
}

#endif

template <typename T>
bool readValue(std::ifstream& input, T& value) {
    return bool(input.read(reinterpret_cast<char*>(&value), sizeof(T)));
}

bool readFloats(std::ifstream& input, std::vector<float>& values, size_t count) {
    values.resize(count);
    return bool(input.read(reinterpret_cast<char*>(values.data()), std::streamsize(count * sizeof(float))));
}

} // namespace

bool load(const std::string& path) {
    auto fail = [] {
        model = Model{};
        return false;
    };

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return fail();

    char magic[8];
    uint32_t version = 0;
    uint32_t layoutId = 0;
    uint32_t featureDim = 0;
    uint32_t hidden1 = 0;
    uint32_t hidden2 = 0;
    if (!input.read(magic, sizeof(magic)) ||
        std::memcmp(magic, MAGIC, sizeof(magic)) != 0 ||
        !readValue(input, version)) {
        return fail();
    }

    if (version == 1) {
        if (!readValue(input, featureDim) ||
            !readValue(input, hidden1) ||
            !readValue(input, hidden2)) {
            return fail();
        }
        layoutId = featureDim == NEW32_FEATURE_DIM ? uint32_t(FeatureLayout::New32) : uint32_t(FeatureLayout::Legacy);
    } else if (version == 2) {
        if (!readValue(input, layoutId) ||
            !readValue(input, featureDim) ||
            !readValue(input, hidden1) ||
            !readValue(input, hidden2)) {
            return fail();
        }
    } else {
        return fail();
    }

    FeatureLayout layout = FeatureLayout::Legacy;
    if (layoutId == uint32_t(FeatureLayout::Legacy)) {
        layout = FeatureLayout::Legacy;
    } else if (layoutId == uint32_t(FeatureLayout::New32)) {
        layout = FeatureLayout::New32;
    } else if (layoutId == uint32_t(FeatureLayout::New64)) {
        layout = FeatureLayout::New64;
    } else {
        return fail();
    }

    bool layoutMatches =
        (layout == FeatureLayout::Legacy && (featureDim == MIN_FEATURE_DIM || featureDim == NEW32_FEATURE_DIM)) ||
        (layout == FeatureLayout::New32 && featureDim == NEW32_FEATURE_DIM) ||
        (layout == FeatureLayout::New64 && featureDim == NEW64_FEATURE_DIM);
    if (!layoutMatches ||
        hidden1 == 0 ||
        hidden2 == 0 ||
        int(hidden1) > MAX_HIDDEN ||
        int(hidden2) > MAX_HIDDEN) {
        return fail();
    }

    Model next;
    next.input = int(featureDim);
    next.hidden1 = int(hidden1);
    next.hidden2 = int(hidden2);
    next.layout = layout;
    next.path = path;

    if (!readFloats(input, next.w1, size_t(next.hidden1) * next.input) ||
        !readFloats(input, next.b1, next.hidden1) ||
        !readFloats(input, next.w2, size_t(next.hidden2) * next.hidden1) ||
        !readFloats(input, next.b2, next.hidden2) ||
        !readFloats(input, next.w3, next.hidden2) ||
        !readValue(input, next.b3)) {
        return fail();
    }

    model = std::move(next);
    return true;
}

bool isLoaded() {
    return !model.w1.empty() &&
        ((model.layout == FeatureLayout::Legacy && (model.input == MIN_FEATURE_DIM || model.input == NEW32_FEATURE_DIM)) ||
         (model.layout == FeatureLayout::New32 && model.input == NEW32_FEATURE_DIM) ||
         (model.layout == FeatureLayout::New64 && model.input == NEW64_FEATURE_DIM));
}

const std::string& loadedPath() {
    return model.path;
}

int scoreMoveImpl(Position& pos, Move move, int scale, const PositionFeatures& pf,
                  AttackCache& ac) {
    alignas(32) float features[MAX_FEATURE_DIM] = {};
    if (model.layout == FeatureLayout::New64)
        fillNew64FeaturesWithCtx(pos, move, pf, ac, features);
    else if (model.layout == FeatureLayout::New32)
        fillNew32FeaturesWithCtx(pos, move, pf, ac, features);
    else
        fillFeaturesWithCtx(pos, move, pf, ac, features, model.input);

    alignas(32) float h1[MAX_HIDDEN];
    layerForward(model.w1.data(), model.b1.data(), features, model.input, model.hidden1, h1);
    alignas(32) float h2[MAX_HIDDEN];
    layerForward(model.w2.data(), model.b2.data(), h1, model.hidden1, model.hidden2, h2);
    float out = denseDotPlusBias(model.w3.data(), model.b3, h2, model.hidden2);

    int bonus = int(std::lround(out * float(scale)));
    return std::clamp(bonus, -20000, 20000);
}

int scoreMove(Position& pos, Move move, int scale) {
    if (!isLoaded() || scale == 0)
        return 0;
    PositionFeatures pf = computePositionFeatures(pos);
    AttackCache ac;
    ac.reset();
    return scoreMoveImpl(pos, move, scale, pf, ac);
}

// Two-phase scoring: extract all features into a contiguous [count, F]
// buffer, then run the MLP forward per move. Same op count as the
// original per-move loop, but lets the timer split feature_ns from
// forward_ns and gives the matmul kernel contiguous reads.
void scoreMoves(Position& pos, const Move* moves, int count, int scale, int* outBonuses) {
    if (!isLoaded() || scale == 0) {
        std::fill_n(outBonuses, count, 0);
        return;
    }
    PositionFeatures pf = computePositionFeatures(pos);
    AttackCache ac;
    ac.reset();
    const int F = model.input;

    auto t0 = std::chrono::steady_clock::now();
    alignas(32) float X[MAX_MOVES * MAX_FEATURE_DIM] = {};
    for (int i = 0; i < count; ++i) {
        float* row = X + size_t(i) * MAX_FEATURE_DIM;
        if (model.layout == FeatureLayout::New64)
            fillNew64FeaturesWithCtx(pos, moves[i], pf, ac, row);
        else if (model.layout == FeatureLayout::New32)
            fillNew32FeaturesWithCtx(pos, moves[i], pf, ac, row);
        else
            fillFeaturesWithCtx(pos, moves[i], pf, ac, row, F);
    }
    auto t1 = std::chrono::steady_clock::now();

    alignas(32) float h1[MAX_HIDDEN];
    alignas(32) float h2[MAX_HIDDEN];
    for (int i = 0; i < count; ++i) {
        const float* xi = X + size_t(i) * MAX_FEATURE_DIM;
        layerForward(model.w1.data(), model.b1.data(), xi, F, model.hidden1, h1);
        layerForward(model.w2.data(), model.b2.data(), h1, model.hidden1, model.hidden2, h2);
        float out = denseDotPlusBias(model.w3.data(), model.b3, h2, model.hidden2);
        int bonus = int(std::lround(out * float(scale)));
        outBonuses[i] = std::clamp(bonus, -20000, 20000);
    }
    auto t2 = std::chrono::steady_clock::now();

    g_perf.featureNanos.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()),
        std::memory_order_relaxed);
    g_perf.forwardNanos.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count()),
        std::memory_order_relaxed);
    g_perf.calls.fetch_add(1, std::memory_order_relaxed);
}

// --- v2 (RINPOL2) ---

namespace {

// RINPOL2 layout (little-endian):
//   '\0' '2' 'L' 'O' 'P' 'N' 'I' 'R'  (magic 'RINPOL2\0' written as text)
//   u32 version = 1
//   u32 layout_id      (same enum as v1)
//   u32 feature_dim
//   u32 hidden1
//   u32 hidden2
//   u32 buckets        (number of move-class calibration entries; 4 expected)
//   u32 phases         (number of phase calibration entries; 0 = none)
//   f32[h1*F]  w1
//   f32[h1]    b1
//   f32[h2*h1] w2
//   f32[h2]    b2
//   f32[h2]    w3
//   f32        b3
//   f32[buckets]  bucket_scale  (multiplied with engine scale, per move class)
//   f32[buckets]  bucket_bias   (added to model output, per move class)
//   f32[phases]   phase_scale   (multiplied with engine scale, per phase)
constexpr char MAGIC2[8] = {'R', 'I', 'N', 'P', 'O', 'L', '2', '\0'};

// Bucket indices used by both training and engine. Order matters:
// the binary lays out bucket vectors in this order.
enum Bucket : int {
    BUCKET_QUIET    = 0,
    BUCKET_CAPTURE  = 1,
    BUCKET_CHECK    = 2,
    BUCKET_PROMO    = 3,
    BUCKET_COUNT    = 4,
};

struct ModelV2 {
    int input = 0;
    int hidden1 = 0;
    int hidden2 = 0;
    FeatureLayout layout = FeatureLayout::Legacy;
    int buckets = 0;
    int phases  = 0;
    std::vector<float> w1, b1, w2, b2, w3;
    float b3 = 0.0f;
    std::vector<float> bucketScale;  // length == buckets
    std::vector<float> bucketBias;   // length == buckets
    std::vector<float> phaseScale;   // length == phases (0 ok)
    std::string path;
};

ModelV2 modelV2;

int phaseIndex(const PositionFeatures& pf) {
    // pieceCount is in [0,1]; 32 pieces -> 1.0, 0 pieces -> 0.0.
    // Map to {opening, middlegame, endgame} for 3-phase calibration.
    if (modelV2.phases <= 1) return 0;
    if (modelV2.phases == 2) return pf.pieceCount > 0.5f ? 0 : 1;
    if (pf.pieceCount > 0.78f) return 0;          // opening
    if (pf.pieceCount > 0.42f) return 1;          // middlegame
    return std::min(modelV2.phases - 1, 2);       // endgame
}

} // namespace

bool loadV2(const std::string& path) {
    auto fail = [] {
        modelV2 = ModelV2{};
        return false;
    };

    std::ifstream input(path, std::ios::binary);
    if (!input) return fail();

    char magic[8];
    if (!input.read(magic, sizeof(magic)) ||
        std::memcmp(magic, MAGIC2, sizeof(magic)) != 0)
        return fail();

    uint32_t version = 0, layoutId = 0, featureDim = 0, hidden1 = 0, hidden2 = 0;
    uint32_t buckets = 0, phases = 0;
    if (!readValue(input, version) || version != 1) return fail();
    if (!readValue(input, layoutId) ||
        !readValue(input, featureDim) ||
        !readValue(input, hidden1) ||
        !readValue(input, hidden2) ||
        !readValue(input, buckets) ||
        !readValue(input, phases))
        return fail();

    FeatureLayout layout;
    if (layoutId == uint32_t(FeatureLayout::Legacy))      layout = FeatureLayout::Legacy;
    else if (layoutId == uint32_t(FeatureLayout::New32))  layout = FeatureLayout::New32;
    else if (layoutId == uint32_t(FeatureLayout::New64))  layout = FeatureLayout::New64;
    else return fail();

    bool dimOk =
        (layout == FeatureLayout::Legacy && (featureDim == MIN_FEATURE_DIM || featureDim == NEW32_FEATURE_DIM)) ||
        (layout == FeatureLayout::New32  && featureDim == NEW32_FEATURE_DIM) ||
        (layout == FeatureLayout::New64  && featureDim == NEW64_FEATURE_DIM);
    if (!dimOk || hidden1 == 0 || hidden2 == 0 ||
        int(hidden1) > MAX_HIDDEN || int(hidden2) > MAX_HIDDEN ||
        buckets == 0 || buckets > 16 || phases > 16)
        return fail();

    ModelV2 next;
    next.input   = int(featureDim);
    next.hidden1 = int(hidden1);
    next.hidden2 = int(hidden2);
    next.layout  = layout;
    next.buckets = int(buckets);
    next.phases  = int(phases);
    next.path    = path;

    if (!readFloats(input, next.w1, size_t(next.hidden1) * next.input) ||
        !readFloats(input, next.b1, next.hidden1) ||
        !readFloats(input, next.w2, size_t(next.hidden2) * next.hidden1) ||
        !readFloats(input, next.b2, next.hidden2) ||
        !readFloats(input, next.w3, next.hidden2) ||
        !readValue(input, next.b3) ||
        !readFloats(input, next.bucketScale, next.buckets) ||
        !readFloats(input, next.bucketBias,  next.buckets))
        return fail();
    if (next.phases > 0) {
        if (!readFloats(input, next.phaseScale, next.phases))
            return fail();
    }

    modelV2 = std::move(next);
    return true;
}

bool isV2Loaded() {
    return !modelV2.w1.empty();
}

const std::string& loadedV2Path() {
    return modelV2.path;
}

void scoreMovesV2(Position& pos, const Move* moves, int count,
                  int scale, int quietScale, int endgameScale,
                  int bonusClamp, int* outBonuses) {
    if (!isV2Loaded() || (scale == 0 && quietScale == 0)) {
        std::fill_n(outBonuses, count, 0);
        return;
    }

    int clampLimit = std::clamp(bonusClamp, 1000, 60000);
    PositionFeatures pf = computePositionFeatures(pos);
    AttackCache ac;
    ac.reset();
    int phase = phaseIndex(pf);
    float phaseMul = (modelV2.phases > 0) ? modelV2.phaseScale[phase] : 1.0f;
    if (phase == 2)
        phaseMul *= float(endgameScale) / 100.0f;
    const int F = modelV2.input;

    auto t0 = std::chrono::steady_clock::now();
    alignas(32) float X[MAX_MOVES * MAX_FEATURE_DIM] = {};
    int buckets[MAX_MOVES];
    bool isQuiet[MAX_MOVES];
    for (int i = 0; i < count; ++i) {
        float* row = X + size_t(i) * MAX_FEATURE_DIM;
        if (modelV2.layout == FeatureLayout::New64)
            fillNew64FeaturesWithCtx(pos, moves[i], pf, ac, row);
        else if (modelV2.layout == FeatureLayout::New32)
            fillNew32FeaturesWithCtx(pos, moves[i], pf, ac, row);
        else
            fillFeaturesWithCtx(pos, moves[i], pf, ac, row, F);

        Piece captured = (moves[i].flag() == FLAG_ENPASSANT)
                           ? makePiece(~pf.us, PAWN)
                           : pos.pieceOn(moves[i].to());
        bool isCapture = captured != NO_PIECE;
        bool isPromo   = moves[i].flag() == FLAG_PROMOTION;
        int bucket = BUCKET_QUIET;
        if (isPromo)        bucket = BUCKET_PROMO;
        else if (isCapture) bucket = BUCKET_CAPTURE;
        buckets[i] = bucket;
        isQuiet[i] = !isCapture && !isPromo;
    }
    auto t1 = std::chrono::steady_clock::now();

    alignas(32) float h1[MAX_HIDDEN];
    alignas(32) float h2[MAX_HIDDEN];
    for (int i = 0; i < count; ++i) {
        const float* xi = X + size_t(i) * MAX_FEATURE_DIM;
        layerForward(modelV2.w1.data(), modelV2.b1.data(), xi, F, modelV2.hidden1, h1);
        layerForward(modelV2.w2.data(), modelV2.b2.data(), h1, modelV2.hidden1, modelV2.hidden2, h2);
        float raw = denseDotPlusBias(modelV2.w3.data(), modelV2.b3, h2, modelV2.hidden2);
        int b = buckets[i];
        float bScale = (b < modelV2.buckets) ? modelV2.bucketScale[b] : 1.0f;
        float bBias  = (b < modelV2.buckets) ? modelV2.bucketBias[b]  : 0.0f;
        int engineScale = isQuiet[i] ? quietScale : scale;
        float scaled = (raw + bBias) * bScale * phaseMul;
        int bonus = int(std::lround(scaled * float(engineScale)));
        outBonuses[i] = std::clamp(bonus, -clampLimit, clampLimit);
    }
    auto t2 = std::chrono::steady_clock::now();

    g_perf.featureNanos.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()),
        std::memory_order_relaxed);
    g_perf.forwardNanos.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count()),
        std::memory_order_relaxed);
    g_perf.calls.fetch_add(1, std::memory_order_relaxed);
}

PerfCounters readPerfCounters() {
    PerfCounters out;
    out.featureNanos = g_perf.featureNanos.load(std::memory_order_relaxed);
    out.forwardNanos = g_perf.forwardNanos.load(std::memory_order_relaxed);
    out.calls        = g_perf.calls.load(std::memory_order_relaxed);
    return out;
}

void resetPerfCounters() {
    g_perf.featureNanos.store(0, std::memory_order_relaxed);
    g_perf.forwardNanos.store(0, std::memory_order_relaxed);
    g_perf.calls.store(0, std::memory_order_relaxed);
}

} // namespace Policy
