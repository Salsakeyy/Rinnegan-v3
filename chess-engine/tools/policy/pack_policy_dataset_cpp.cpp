#include "bitboard.h"
#include "magics.h"
#include "movegen.h"
#include "position.h"
#include "see.h"
#include "zobrist.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kLegacyMinFeatureDim = 17;
constexpr int kNew32FeatureDim = 32;
constexpr int kNew64FeatureDim = 64;
constexpr int kMaxFeatureDim = kNew64FeatureDim;
constexpr uint32_t kZip32Max = 0xFFFFFFFFu;
constexpr uint16_t kZip16Max = 0xFFFFu;

enum class FeatureLayout {
    Legacy,
    New32,
    New64,
};

enum class OutputFormat {
    Auto,
    Npz,
    NpyDir,
};

enum class FeatureDtype {
    Float32,
    Float16,
};

struct Options {
    std::filesystem::path input;
    std::filesystem::path output;
    int limit = 0;
    int featureDim = kNew32FeatureDim;
    FeatureLayout layout = FeatureLayout::Legacy;
    FeatureDtype featureDtype = FeatureDtype::Float32;
    OutputFormat outputFormat = OutputFormat::Auto;
    bool quietOnly = false;
    bool writeLabels = true;
    bool writeSidecars = true;
    int progressEvery = 50000;
};

struct AcceptedPosition {
    std::string fen;
    std::string bestmove;
    int moveCount = 0;
    int targetIndex = -1;
};

struct Plan {
    std::vector<AcceptedPosition> positions;
    std::vector<int64_t> offsets{0};
    std::vector<int64_t> targets;
    std::vector<int64_t> bucketFlags;
    int64_t candidates = 0;
    int64_t skipped = 0;
};

struct PositionFeatures {
    Color us = WHITE;
    float pieceCount = 0.0f;
    float matBalance = 0.0f;
    float fullmovePhase = 0.0f;
    int enemyKingFile = 0;
    int enemyKingRank = 0;
    bool hasEnemyKing = false;
    bool wasInCheck = false;
};

struct AttackCache {
    std::array<int8_t, 64> attackedFrom{};
    void reset() { attackedFrom.fill(-1); }
};

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

float clampFloat(float value, float lo, float hi) {
    return std::max(lo, std::min(hi, value));
}

std::string usage() {
    return
        "usage: pack_policy_dataset_cpp --input labels.jsonl --output dataset.npz [options]\n"
        "\n"
        "options:\n"
        "  --limit N                  pack at most N accepted positions\n"
        "  --feature-dim N            legacy feature dim, 17 or 32 (default: 32)\n"
        "  --feature-layout LAYOUT    legacy, new32, or new64 (default: legacy)\n"
        "  --feature-dtype DTYPE      f32 or f16 feature storage (default: f32)\n"
        "  --quiet-only               keep only quiet non-promotion candidates\n"
        "  --output-format FORMAT     auto, npz, or npy-dir (default: auto)\n"
        "  --write-labels/--no-labels write dense one-hot labels array (default: write)\n"
        "  --write-sidecars/--no-sidecars write FEN/bestmove sidecars (default: write)\n"
        "  --progress-every N         print progress every N accepted positions\n"
        "  --compressed/--no-compressed accepted for Python packer CLI compatibility; output is stored\n";
}

int parseInt(std::string_view value, const char* flag) {
    try {
        size_t used = 0;
        int parsed = std::stoi(std::string(value), &used);
        if (used != value.size())
            throw std::invalid_argument("trailing characters");
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("invalid integer for ") + flag + ": " + std::string(value));
    }
}

Options parseOptions(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        auto needValue = [&](const char* flag) -> std::string_view {
            if (++i >= argc)
                throw std::runtime_error(std::string("missing value for ") + flag);
            return argv[i];
        };

        if (arg == "--input") {
            opt.input = std::filesystem::path(std::string(needValue("--input")));
        } else if (arg == "--output") {
            opt.output = std::filesystem::path(std::string(needValue("--output")));
        } else if (arg == "--limit") {
            opt.limit = parseInt(needValue("--limit"), "--limit");
        } else if (arg == "--feature-dim") {
            opt.featureDim = parseInt(needValue("--feature-dim"), "--feature-dim");
        } else if (arg == "--feature-layout") {
            std::string value(needValue("--feature-layout"));
            if (value == "legacy") {
                opt.layout = FeatureLayout::Legacy;
            } else if (value == "new32") {
                opt.layout = FeatureLayout::New32;
                opt.featureDim = kNew32FeatureDim;
            } else if (value == "new64") {
                opt.layout = FeatureLayout::New64;
                opt.featureDim = kNew64FeatureDim;
            } else {
                throw std::runtime_error("unsupported feature layout: " + value);
            }
        } else if (arg == "--feature-dtype") {
            std::string value(needValue("--feature-dtype"));
            if (value == "f32" || value == "float32") {
                opt.featureDtype = FeatureDtype::Float32;
            } else if (value == "f16" || value == "float16") {
                opt.featureDtype = FeatureDtype::Float16;
            } else {
                throw std::runtime_error("unsupported feature dtype: " + value);
            }
        } else if (arg == "--quiet-only") {
            opt.quietOnly = true;
        } else if (arg == "--no-quiet-only") {
            opt.quietOnly = false;
        } else if (arg == "--output-format") {
            std::string value(needValue("--output-format"));
            if (value == "auto") {
                opt.outputFormat = OutputFormat::Auto;
            } else if (value == "npz") {
                opt.outputFormat = OutputFormat::Npz;
            } else if (value == "npy-dir") {
                opt.outputFormat = OutputFormat::NpyDir;
            } else {
                throw std::runtime_error("unsupported output format: " + value);
            }
        } else if (arg == "--write-labels") {
            opt.writeLabels = true;
        } else if (arg == "--no-labels" || arg == "--no-write-labels") {
            opt.writeLabels = false;
        } else if (arg == "--write-sidecars") {
            opt.writeSidecars = true;
        } else if (arg == "--no-sidecars" || arg == "--no-write-sidecars") {
            opt.writeSidecars = false;
        } else if (arg == "--progress-every") {
            opt.progressEvery = parseInt(needValue("--progress-every"), "--progress-every");
        } else if (arg == "--compressed" || arg == "--no-compressed") {
            // Compatibility with the Python packer. This writer stores uncompressed NPZ entries.
        } else if (arg == "--help" || arg == "-h") {
            std::cout << usage();
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + std::string(arg));
        }
    }

    if (opt.input.empty())
        throw std::runtime_error("missing --input");
    if (opt.output.empty())
        throw std::runtime_error("missing --output");
    if (opt.limit < 0)
        throw std::runtime_error("--limit must be >= 0");
    if (opt.progressEvery < 0)
        throw std::runtime_error("--progress-every must be >= 0");
    if (opt.layout == FeatureLayout::Legacy &&
        opt.featureDim != kLegacyMinFeatureDim && opt.featureDim != kNew32FeatureDim) {
        throw std::runtime_error("legacy --feature-dim must be 17 or 32");
    }
    if (opt.outputFormat == OutputFormat::Auto) {
        opt.outputFormat = opt.output.extension() == ".npz" ? OutputFormat::Npz : OutputFormat::NpyDir;
    }
    return opt;
}

bool extractJsonString(const std::string& line, std::string_view key, std::string& out) {
    out.clear();
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = line.find(needle);
    if (pos == std::string::npos)
        return false;
    pos = line.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return false;
    ++pos;
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])))
        ++pos;
    if (pos >= line.size() || line[pos] != '"')
        return false;
    ++pos;

    while (pos < line.size()) {
        char c = line[pos++];
        if (c == '"')
            return true;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (pos >= line.size())
            return false;
        char escaped = line[pos++];
        switch (escaped) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default: out.push_back(escaped); break;
        }
    }
    return false;
}

std::string firstPvMove(const std::string& pv) {
    size_t start = 0;
    while (start < pv.size() && std::isspace(static_cast<unsigned char>(pv[start])))
        ++start;
    size_t end = start;
    while (end < pv.size() && !std::isspace(static_cast<unsigned char>(pv[end])))
        ++end;
    return pv.substr(start, end - start);
}

bool recordFenAndTeacher(const std::string& line, std::string& fen, std::string& teacher) {
    if (!extractJsonString(line, "fen", fen) || fen.empty())
        return false;
    std::string bestmove;
    if (extractJsonString(line, "bestmove", bestmove)) {
        teacher = bestmove;
    }
    if (teacher.empty() || teacher == "0000") {
        std::string pv;
        if (extractJsonString(line, "pv", pv))
            teacher = firstPvMove(pv);
    }
    return !teacher.empty() && teacher != "0000";
}

bool isCapture(const Position& pos, Move move) {
    return move.flag() == FLAG_ENPASSANT || pos.pieceOn(move.to()) != NO_PIECE;
}

bool isQuietPolicyMove(const Position& pos, Move move) {
    return !isCapture(pos, move) && move.flag() != FLAG_PROMOTION;
}

int64_t bucketFlagsForMove(Position& pos, Move move) {
    constexpr int64_t Quiet = 1;
    constexpr int64_t Capture = 2;
    constexpr int64_t Check = 4;
    constexpr int64_t Promotion = 8;

    int64_t flags = 0;
    bool capture = isCapture(pos, move);
    bool promotion = move.flag() == FLAG_PROMOTION;
    if (!capture && !promotion)
        flags |= Quiet;
    if (capture)
        flags |= Capture;
    if (promotion)
        flags |= Promotion;

    StateInfo state;
    pos.makeMove(move, state);
    if (pos.inCheck())
        flags |= Check;
    pos.unmakeMove(move);
    return flags;
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
    return clampFloat(float(balance) / 2000.0f, -1.0f, 1.0f);
}

float centrality(int file, int rank) {
    return (3.5f - std::max(std::abs(float(file) - 3.5f), std::abs(float(rank) - 3.5f))) / 3.5f;
}

int staticExchangeScore(const Position& pos, Move move) {
    if (SEE::seeGE(pos, move, 0)) {
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
    if (SEE::seeGE(pos, move, -100)) return -50;
    if (SEE::seeGE(pos, move, -300)) return -100;
    if (SEE::seeGE(pos, move, -500)) return -300;
    if (SEE::seeGE(pos, move, -1000)) return -500;
    return -1000;
}

PositionFeatures computePositionFeatures(const Position& pos) {
    PositionFeatures pf;
    pf.us = pos.sideToMove();
    pf.pieceCount = float(BB::popcount(pos.allPieces())) / 32.0f;
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
    int best = std::numeric_limits<int>::max();
    while (attackers) {
        Square attacker = BB::poplsb(attackers);
        Piece piece = pos.pieceOn(attacker);
        if (piece != NO_PIECE)
            best = std::min(best, pieceValue(pieceType(piece)));
    }
    return best == std::numeric_limits<int>::max() ? 0 : best;
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

void fillLegacyFeatures(Position& pos, Move move, const PositionFeatures& pf,
                        AttackCache& cache, float* out, int featureDim) {
    std::fill(out, out + featureDim, 0.0f);

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

    bool capture = captured != NO_PIECE;
    bool promotion = move.flag() == FLAG_PROMOTION;
    int moverValue = mover == NO_PIECE ? 0 : pieceValue(pieceType(mover));
    int capturedValue = captured == NO_PIECE ? 0 : pieceValue(pieceType(captured));

    StateInfo state;
    pos.makeMove(move, state);
    bool givesCheck = pos.inCheck();
    bool attackedTo = (featureDim == kNew32FeatureDim) ? pos.isSquareAttacked(move.to(), pos.sideToMove()) : false;
    pos.unmakeMove(move);

    out[0] = float(fromFile) / 7.0f;
    out[1] = float(fromRank) / 7.0f;
    out[2] = float(toFile) / 7.0f;
    out[3] = float(toRank) / 7.0f;
    out[4] = float(toFile - fromFile) / 7.0f;
    out[5] = float(toRank - fromRank) / 7.0f;
    out[6] = mover == NO_PIECE ? 0.0f : float(int(pieceType(mover)) + 1) / 6.0f;
    out[7] = captured == NO_PIECE ? 0.0f : float(int(pieceType(captured)) + 1) / 6.0f;
    out[8] = promotion ? float(int(move.promoPiece()) + 1) / 6.0f : 0.0f;
    out[9] = capture ? 1.0f : 0.0f;
    out[10] = promotion ? 1.0f : 0.0f;
    out[11] = move.flag() == FLAG_CASTLING ? 1.0f : 0.0f;
    out[12] = move.flag() == FLAG_ENPASSANT ? 1.0f : 0.0f;
    out[13] = givesCheck ? 1.0f : 0.0f;
    out[14] = pf.pieceCount;
    out[15] = pf.matBalance;
    out[16] = pf.fullmovePhase;

    if (featureDim == kLegacyMinFeatureDim)
        return;

    float fromCentrality = centrality(fromFile, fromRank);
    float toCentrality = centrality(toFile, toRank);
    int see = (capture || promotion || attackedTo) ? staticExchangeScore(pos, move) : 0;

    int8_t cached = cache.attackedFrom[int(move.from())];
    bool attackedFrom;
    if (cached < 0) {
        attackedFrom = pos.isSquareAttacked(move.from(), ~pf.us);
        cache.attackedFrom[int(move.from())] = attackedFrom ? 1 : 0;
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
    out[23] = clampFloat(float(see) / 1000.0f, -2.0f, 2.0f);
    out[24] = see >= 0 ? 1.0f : 0.0f;
    out[25] = see < 0 ? 1.0f : 0.0f;
    out[26] = (mover != NO_PIECE && pieceType(mover) == PAWN && std::abs(toRank - fromRank) == 2) ? 1.0f : 0.0f;
    out[27] = attackedFrom ? 1.0f : 0.0f;
    out[28] = attackedTo ? 1.0f : 0.0f;
    out[29] = pf.wasInCheck ? 1.0f : 0.0f;
    out[30] = advancement;
    out[31] = float(std::abs(toFile - fromFile) + std::abs(toRank - fromRank)) / 14.0f;
}

void setOneHotPiece(PieceType pt, const PieceType* values, int count, float* out) {
    for (int i = 0; i < count; ++i)
        out[i] = (pt == values[i]) ? 1.0f : 0.0f;
}

void fillNew32Features(Position& pos, Move move, const PositionFeatures& pf,
                       AttackCache& cache, float* out) {
    std::fill(out, out + kNew32FeatureDim, 0.0f);

    Color us = pf.us;
    Piece mover = pos.pieceOn(move.from());
    Piece captured = (move.flag() == FLAG_ENPASSANT) ? makePiece(~us, PAWN) : pos.pieceOn(move.to());
    PieceType moverType = mover == NO_PIECE ? NO_PIECE_TYPE : pieceType(mover);
    PieceType capturedType = captured == NO_PIECE ? NO_PIECE_TYPE : pieceType(captured);

    int fromFile = fileOf(move.from());
    int fromRank = rankOf(move.from());
    int toFile = fileOf(move.to());
    int toRank = rankOf(move.to());
    if (us == BLACK) {
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
    float mobilityDelta = 0.0f;

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
    mobilityDelta = clampFloat(float(afterMobility - beforeMobility) / 28.0f, -1.0f, 1.0f);

    bool capture = captured != NO_PIECE;
    bool promotion = move.flag() == FLAG_PROMOTION;
    int see = (capture || promotion || attackedTo) ? staticExchangeScore(pos, move) : 0;
    int8_t cached = cache.attackedFrom[int(move.from())];
    bool attackedFrom;
    if (cached < 0) {
        attackedFrom = pos.isSquareAttacked(move.from(), ~us);
        cache.attackedFrom[int(move.from())] = attackedFrom ? 1 : 0;
    } else {
        attackedFrom = bool(cached);
    }

    static constexpr PieceType moverTypes[6] = {PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING};
    static constexpr PieceType capturedTypes[5] = {PAWN, KNIGHT, BISHOP, ROOK, QUEEN};

    out[0] = float(fromFile) / 7.0f;
    out[1] = float(fromRank) / 7.0f;
    out[2] = float(toFile) / 7.0f;
    out[3] = float(toRank) / 7.0f;
    out[4] = float(toRank - fromRank) / 7.0f;
    setOneHotPiece(moverType, moverTypes, 6, out + 5);
    setOneHotPiece(capturedType, capturedTypes, 5, out + 11);
    out[16] = promotion ? float(int(move.promoPiece()) + 1) / 6.0f : 0.0f;
    out[17] = capture ? 1.0f : 0.0f;
    out[18] = promotion ? 1.0f : 0.0f;
    out[19] = move.flag() == FLAG_CASTLING ? 1.0f : 0.0f;
    out[20] = givesCheck ? 1.0f : 0.0f;
    out[21] = pf.matBalance;
    out[22] = pf.pieceCount;
    out[23] = clampFloat(float(see) / 1000.0f, -2.0f, 2.0f);
    out[24] = attackedFrom ? 1.0f : 0.0f;
    out[25] = float(enemyAttacker) / 900.0f;
    out[26] = float(ownDefender) / 900.0f;
    out[27] = kingDistance;
    out[28] = inKingRing;
    out[29] = attacksMoreValuable ? 1.0f : 0.0f;
    out[30] = attacksUndefended ? 1.0f : 0.0f;
    out[31] = mobilityDelta;
}

void fillNew64Features(Position& pos, Move move, const PositionFeatures& pf,
                       AttackCache& cache, float* out) {
    std::fill(out, out + kNew64FeatureDim, 0.0f);
    fillNew32Features(pos, move, pf, cache, out);

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
    out[59] = clampFloat(promotionDistance, 0.0f, 1.0f);
    out[60] = doublePawnPush ? 1.0f : 0.0f;
    out[61] = developmentMove ? 1.0f : 0.0f;
    out[62] = rookOpen ? 1.0f : 0.0f;
    out[63] = rookSemiOpen ? 1.0f : 0.0f;
}

uint32_t crc32Update(uint32_t crc, const void* data, size_t len) {
    static uint32_t table[256];
    static bool initialized = false;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            table[i] = value;
        }
        initialized = true;
    }

    uint32_t value = crc ^ 0xFFFFFFFFu;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i)
        value = table[(value ^ bytes[i]) & 0xFFu] ^ (value >> 8);
    return value ^ 0xFFFFFFFFu;
}

void writeRaw(std::ofstream& out, const void* data, size_t len) {
    out.write(static_cast<const char*>(data), std::streamsize(len));
    if (!out)
        throw std::runtime_error("failed writing output");
}

void writeU16(std::ofstream& out, uint16_t value) {
    char bytes[2] = {
        char(value & 0xFFu),
        char((value >> 8) & 0xFFu),
    };
    writeRaw(out, bytes, sizeof(bytes));
}

void writeU32(std::ofstream& out, uint32_t value) {
    char bytes[4] = {
        char(value & 0xFFu),
        char((value >> 8) & 0xFFu),
        char((value >> 16) & 0xFFu),
        char((value >> 24) & 0xFFu),
    };
    writeRaw(out, bytes, sizeof(bytes));
}

void writeU64(std::ofstream& out, uint64_t value) {
    char bytes[8] = {
        char(value & 0xFFu),
        char((value >> 8) & 0xFFu),
        char((value >> 16) & 0xFFu),
        char((value >> 24) & 0xFFu),
        char((value >> 32) & 0xFFu),
        char((value >> 40) & 0xFFu),
        char((value >> 48) & 0xFFu),
        char((value >> 56) & 0xFFu),
    };
    writeRaw(out, bytes, sizeof(bytes));
}

std::vector<char> npyHeader(std::string_view descr, const std::vector<int64_t>& shape) {
    std::string shapeText = "(";
    for (size_t i = 0; i < shape.size(); ++i) {
        if (i)
            shapeText += ", ";
        shapeText += std::to_string(shape[i]);
    }
    if (shape.size() == 1)
        shapeText += ",";
    shapeText += ")";

    std::string dict = "{'descr': '" + std::string(descr) +
                       "', 'fortran_order': False, 'shape': " + shapeText + ", }";
    const size_t preamble = 10;
    size_t padding = 16 - ((preamble + dict.size() + 1) % 16);
    if (padding == 16)
        padding = 0;
    dict.append(padding, ' ');
    dict.push_back('\n');
    if (dict.size() > kZip16Max)
        throw std::runtime_error("npy header too large for v1.0");

    std::vector<char> header;
    header.reserve(preamble + dict.size());
    const unsigned char magic[] = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    header.insert(header.end(), reinterpret_cast<const char*>(magic),
                  reinterpret_cast<const char*>(magic) + sizeof(magic));
    header.push_back(char(1));
    header.push_back(char(0));
    uint16_t headerLen = uint16_t(dict.size());
    header.push_back(char(headerLen & 0xFFu));
    header.push_back(char((headerLen >> 8) & 0xFFu));
    header.insert(header.end(), dict.begin(), dict.end());
    return header;
}

uint16_t floatToHalf(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    uint32_t sign = (bits >> 16) & 0x8000u;
    int exp = int((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = bits & 0x7FFFFFu;

    if ((bits & 0x7FFFFFFFu) == 0)
        return uint16_t(sign);
    if (exp <= 0) {
        if (exp < -10)
            return uint16_t(sign);
        mant |= 0x800000u;
        int shift = 14 - exp;
        uint32_t rounded = mant + (1u << (shift - 1));
        return uint16_t(sign | (rounded >> shift));
    }
    if (exp >= 31)
        return uint16_t(sign | 0x7C00u);

    mant += 0x1000u;
    if (mant & 0x800000u) {
        mant = 0;
        ++exp;
        if (exp >= 31)
            return uint16_t(sign | 0x7C00u);
    }
    return uint16_t(sign | (uint32_t(exp) << 10) | (mant >> 13));
}

size_t featureElementSize(FeatureDtype dtype) {
    return dtype == FeatureDtype::Float16 ? sizeof(uint16_t) : sizeof(float);
}

std::string_view featureDescr(FeatureDtype dtype) {
    return dtype == FeatureDtype::Float16 ? std::string_view("<f2") : std::string_view("<f4");
}

std::string_view featureDtypeName(FeatureDtype dtype) {
    return dtype == FeatureDtype::Float16 ? std::string_view("float16") : std::string_view("float32");
}

class NpzWriter {
public:
    explicit NpzWriter(const std::filesystem::path& path) : out_(path, std::ios::binary) {
        if (!out_)
            throw std::runtime_error("failed opening output: " + path.string());
    }

    void beginEntry(const std::string& name, uint64_t size) {
        if (open_)
            throw std::runtime_error("internal error: nested zip entry");

        current_ = Entry{};
        current_.name = name;
        current_.size = size;
        current_.offset = tell();
        current_.zip64Local = size >= kZip32Max;
        currentCrc_ = 0;
        open_ = true;

        writeU32(out_, 0x04034b50u);
        writeU16(out_, current_.zip64Local ? 45 : 20);
        writeU16(out_, 0);
        writeU16(out_, 0);
        writeU16(out_, 0);
        writeU16(out_, 0);
        current_.crcPatchOffset = tell();
        writeU32(out_, 0);
        if (current_.zip64Local) {
            writeU32(out_, kZip32Max);
            writeU32(out_, kZip32Max);
        } else {
            writeU32(out_, uint32_t(size));
            writeU32(out_, uint32_t(size));
        }
        writeU16(out_, uint16_t(name.size()));
        writeU16(out_, current_.zip64Local ? 20 : 0);
        writeRaw(out_, name.data(), name.size());
        if (current_.zip64Local) {
            writeU16(out_, 0x0001);
            writeU16(out_, 16);
            writeU64(out_, size);
            writeU64(out_, size);
        }
    }

    void writeData(const void* data, size_t len) {
        if (!open_)
            throw std::runtime_error("internal error: no open zip entry");
        currentCrc_ = crc32Update(currentCrc_, data, len);
        writeRaw(out_, data, len);
    }

    void endEntry() {
        if (!open_)
            throw std::runtime_error("internal error: no open zip entry");
        current_.crc = currentCrc_;

        uint64_t end = tell();
        out_.seekp(std::streamoff(current_.crcPatchOffset), std::ios::beg);
        writeU32(out_, current_.crc);
        out_.seekp(std::streamoff(end), std::ios::beg);
        if (!out_)
            throw std::runtime_error("failed seeking output");

        entries_.push_back(current_);
        open_ = false;
    }

    void finish() {
        if (open_)
            throw std::runtime_error("internal error: zip entry left open");

        uint64_t cdStart = tell();
        bool needsZip64 = false;
        for (const Entry& entry : entries_) {
            bool size64 = entry.size >= kZip32Max;
            bool offset64 = entry.offset >= kZip32Max;
            bool entry64 = size64 || offset64;
            needsZip64 = needsZip64 || entry64;

            writeU32(out_, 0x02014b50u);
            writeU16(out_, entry64 ? 45 : 20);
            writeU16(out_, entry.zip64Local ? 45 : 20);
            writeU16(out_, 0);
            writeU16(out_, 0);
            writeU16(out_, 0);
            writeU16(out_, 0);
            writeU32(out_, entry.crc);
            writeU32(out_, size64 ? kZip32Max : uint32_t(entry.size));
            writeU32(out_, size64 ? kZip32Max : uint32_t(entry.size));
            writeU16(out_, uint16_t(entry.name.size()));

            uint16_t extraSize = 0;
            if (entry64) {
                extraSize = 4;
                if (size64)
                    extraSize += 16;
                if (offset64)
                    extraSize += 8;
            }
            writeU16(out_, extraSize);
            writeU16(out_, 0);
            writeU16(out_, 0);
            writeU16(out_, 0);
            writeU32(out_, 0);
            writeU32(out_, offset64 ? kZip32Max : uint32_t(entry.offset));
            writeRaw(out_, entry.name.data(), entry.name.size());
            if (entry64) {
                writeU16(out_, 0x0001);
                writeU16(out_, uint16_t(extraSize - 4));
                if (size64) {
                    writeU64(out_, entry.size);
                    writeU64(out_, entry.size);
                }
                if (offset64)
                    writeU64(out_, entry.offset);
            }
        }

        uint64_t cdEnd = tell();
        uint64_t cdSize = cdEnd - cdStart;
        needsZip64 = needsZip64 ||
            entries_.size() >= kZip16Max ||
            cdSize >= kZip32Max ||
            cdStart >= kZip32Max;

        uint64_t zip64EocdOffset = 0;
        if (needsZip64) {
            zip64EocdOffset = tell();
            writeU32(out_, 0x06064b50u);
            writeU64(out_, 44);
            writeU16(out_, 45);
            writeU16(out_, 45);
            writeU32(out_, 0);
            writeU32(out_, 0);
            writeU64(out_, entries_.size());
            writeU64(out_, entries_.size());
            writeU64(out_, cdSize);
            writeU64(out_, cdStart);

            writeU32(out_, 0x07064b50u);
            writeU32(out_, 0);
            writeU64(out_, zip64EocdOffset);
            writeU32(out_, 1);
        }

        writeU32(out_, 0x06054b50u);
        writeU16(out_, 0);
        writeU16(out_, 0);
        writeU16(out_, entries_.size() >= kZip16Max ? kZip16Max : uint16_t(entries_.size()));
        writeU16(out_, entries_.size() >= kZip16Max ? kZip16Max : uint16_t(entries_.size()));
        writeU32(out_, cdSize >= kZip32Max ? kZip32Max : uint32_t(cdSize));
        writeU32(out_, cdStart >= kZip32Max ? kZip32Max : uint32_t(cdStart));
        writeU16(out_, 0);
    }

private:
    struct Entry {
        std::string name;
        uint64_t size = 0;
        uint64_t offset = 0;
        uint64_t crcPatchOffset = 0;
        uint32_t crc = 0;
        bool zip64Local = false;
    };

    uint64_t tell() {
        std::streampos pos = out_.tellp();
        if (pos < 0)
            throw std::runtime_error("failed reading output position");
        return uint64_t(pos);
    }

    std::ofstream out_;
    std::vector<Entry> entries_;
    Entry current_;
    uint32_t currentCrc_ = 0;
    bool open_ = false;
};

class NpyFileWriter {
public:
    explicit NpyFileWriter(const std::filesystem::path& path) : out_(path, std::ios::binary) {
        if (!out_)
            throw std::runtime_error("failed opening output: " + path.string());
    }

    void writeData(const void* data, size_t len) {
        writeRaw(out_, data, len);
    }

private:
    std::ofstream out_;
};

bool acceptedMoves(Position& pos, const std::string& teacherUci, bool quietOnly,
                   MoveList& selected, int& targetIndex) {
    MoveList legal;
    MoveGen::generateLegal(pos, legal);

    Move teacher = MOVE_NONE;
    for (int i = 0; i < legal.count; ++i) {
        if (legal[i].toUCI() == teacherUci) {
            teacher = legal[i];
            break;
        }
    }
    if (!teacher)
        return false;

    if (quietOnly && !isQuietPolicyMove(pos, teacher))
        return false;

    selected.count = 0;
    targetIndex = -1;
    for (int i = 0; i < legal.count; ++i) {
        Move move = legal[i];
        if (quietOnly && !isQuietPolicyMove(pos, move))
            continue;
        if (move == teacher)
            targetIndex = selected.count;
        selected.add(move);
    }

    if (quietOnly && selected.count < 2)
        return false;
    return targetIndex >= 0 && selected.count > 0;
}

Plan buildPlan(const Options& opt) {
    std::ifstream input(opt.input);
    if (!input)
        throw std::runtime_error("failed opening input: " + opt.input.string());

    Plan plan;
    if (opt.limit > 0) {
        plan.positions.reserve(size_t(opt.limit));
        plan.offsets.reserve(size_t(opt.limit) + 1);
        plan.targets.reserve(size_t(opt.limit));
    }

    std::string line;
    std::string fen;
    std::string teacher;
    int64_t lines = 0;
    while (std::getline(input, line)) {
        ++lines;
        if (opt.limit > 0 && int(plan.positions.size()) >= opt.limit)
            break;

        fen.clear();
        teacher.clear();
        if (!recordFenAndTeacher(line, fen, teacher)) {
            ++plan.skipped;
            continue;
        }

        Position pos;
        StateInfo root;
        try {
            pos.setFromFen(fen, root);
        } catch (const std::exception&) {
            ++plan.skipped;
            continue;
        }

        MoveList moves;
        int target = -1;
        if (!acceptedMoves(pos, teacher, opt.quietOnly, moves, target)) {
            ++plan.skipped;
            continue;
        }

        plan.candidates += moves.count;
        plan.positions.push_back({pos.fen(), teacher, moves.count, target});
        plan.targets.push_back(target);
        plan.bucketFlags.push_back(bucketFlagsForMove(pos, moves[target]));
        plan.offsets.push_back(plan.candidates);

        if (opt.progressEvery > 0 && plan.positions.size() % size_t(opt.progressEvery) == 0) {
            std::cerr << "planned " << plan.positions.size() << " positions, "
                      << plan.candidates << " candidates; skipped " << plan.skipped << "\n";
        }
    }

    if (plan.positions.empty())
        throw std::runtime_error("no usable policy positions");

    std::cerr << "planned " << plan.positions.size() << " positions, "
              << plan.candidates << " candidates from " << lines
              << " records; skipped " << plan.skipped << "\n";
    return plan;
}

void writeSidecars(const Options& opt, const Plan& plan) {
    std::ofstream fens(opt.output.string() + ".fens.txt");
    if (!fens)
        throw std::runtime_error("failed opening fens sidecar");
    std::ofstream bestmoves(opt.output.string() + ".bestmoves.txt");
    if (!bestmoves)
        throw std::runtime_error("failed opening bestmoves sidecar");
    for (const AcceptedPosition& position : plan.positions) {
        fens << position.fen << '\n';
        bestmoves << position.bestmove << '\n';
    }
}

template <typename Writer>
void writeFeatureRows(Writer& writer, const Options& opt, const Plan& plan) {
    std::array<float, kMaxFeatureDim> features{};
    std::array<uint16_t, kMaxFeatureDim> halfFeatures{};
    for (size_t i = 0; i < plan.positions.size(); ++i) {
        const AcceptedPosition& accepted = plan.positions[i];
        Position pos;
        StateInfo root;
        pos.setFromFen(accepted.fen, root);

        MoveList moves;
        int target = -1;
        if (!acceptedMoves(pos, accepted.bestmove, opt.quietOnly, moves, target) ||
            target != accepted.targetIndex || moves.count != accepted.moveCount) {
            throw std::runtime_error("accepted move set changed while writing features at position " + std::to_string(i));
        }

        PositionFeatures pf = computePositionFeatures(pos);
        AttackCache cache;
        cache.reset();
        for (int moveIndex = 0; moveIndex < moves.count; ++moveIndex) {
            if (opt.layout == FeatureLayout::New64) {
                fillNew64Features(pos, moves[moveIndex], pf, cache, features.data());
            } else if (opt.layout == FeatureLayout::New32) {
                fillNew32Features(pos, moves[moveIndex], pf, cache, features.data());
            } else {
                fillLegacyFeatures(pos, moves[moveIndex], pf, cache, features.data(), opt.featureDim);
            }
            if (opt.featureDtype == FeatureDtype::Float16) {
                for (int feature = 0; feature < opt.featureDim; ++feature)
                    halfFeatures[feature] = floatToHalf(features[feature]);
                writer.writeData(halfFeatures.data(), size_t(opt.featureDim) * sizeof(uint16_t));
            } else {
                writer.writeData(features.data(), size_t(opt.featureDim) * sizeof(float));
            }
        }

        if (opt.progressEvery > 0 && (i + 1) % size_t(opt.progressEvery) == 0) {
            std::cerr << "wrote features for " << (i + 1) << " positions\n";
        }
    }
}

void writeFeatures(NpzWriter& npz, const Options& opt, const Plan& plan,
                   const std::vector<char>& header) {
    uint64_t bytes = uint64_t(header.size()) +
        uint64_t(plan.candidates) * uint64_t(opt.featureDim) * uint64_t(featureElementSize(opt.featureDtype));
    npz.beginEntry("features.npy", bytes);
    npz.writeData(header.data(), header.size());
    writeFeatureRows(npz, opt, plan);
    npz.endEntry();
}

void writeFeaturesNpy(const std::filesystem::path& path, const Options& opt, const Plan& plan,
                      const std::vector<char>& header) {
    NpyFileWriter writer(path);
    writer.writeData(header.data(), header.size());
    writeFeatureRows(writer, opt, plan);
}

template <typename Writer>
void writeLabelValues(Writer& writer, const Plan& plan) {
    for (size_t group = 0; group < plan.targets.size(); ++group) {
        int64_t start = plan.offsets[group];
        int64_t end = plan.offsets[group + 1];
        int64_t target = plan.targets[group];
        for (int64_t i = start; i < end; ++i) {
            float label = (i - start == target) ? 1.0f : 0.0f;
            writer.writeData(&label, sizeof(label));
        }
    }
}

void writeLabels(NpzWriter& npz, const Plan& plan, const std::vector<char>& header) {
    uint64_t bytes = uint64_t(header.size()) + uint64_t(plan.candidates) * sizeof(float);
    npz.beginEntry("labels.npy", bytes);
    npz.writeData(header.data(), header.size());
    writeLabelValues(npz, plan);
    npz.endEntry();
}

void writeLabelsNpy(const std::filesystem::path& path, const Plan& plan,
                    const std::vector<char>& header) {
    NpyFileWriter writer(path);
    writer.writeData(header.data(), header.size());
    writeLabelValues(writer, plan);
}

template <typename T>
void writeVectorArray(NpzWriter& npz, const std::string& name, const std::vector<T>& values,
                      const std::vector<char>& header) {
    uint64_t bytes = uint64_t(header.size()) + uint64_t(values.size()) * sizeof(T);
    npz.beginEntry(name, bytes);
    npz.writeData(header.data(), header.size());
    if (!values.empty())
        npz.writeData(values.data(), values.size() * sizeof(T));
    npz.endEntry();
}

template <typename T>
void writeVectorNpy(const std::filesystem::path& path, const std::vector<T>& values,
                    const std::vector<char>& header) {
    NpyFileWriter writer(path);
    writer.writeData(header.data(), header.size());
    if (!values.empty())
        writer.writeData(values.data(), values.size() * sizeof(T));
}

std::string layoutName(FeatureLayout layout) {
    if (layout == FeatureLayout::New64)
        return "new64";
    return layout == FeatureLayout::New32 ? "new32" : "legacy";
}

std::string outputFormatName(OutputFormat format) {
    return format == OutputFormat::NpyDir ? "npy-dir" : "npz";
}

std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(c));
                out += buf;
            } else {
                out.push_back(c);
            }
            break;
        }
    }
    return out;
}

void writeMetaJson(const std::filesystem::path& path, const Options& opt, const Plan& plan) {
    std::ofstream meta(path);
    if (!meta)
        throw std::runtime_error("failed opening metadata sidecar: " + path.string());
    meta
        << "{\n"
        << "  \"source\": \"" << jsonEscape(opt.input.string()) << "\",\n"
        << "  \"positions\": " << plan.positions.size() << ",\n"
        << "  \"candidates\": " << plan.candidates << ",\n"
        << "  \"feature_dim\": " << opt.featureDim << ",\n"
        << "  \"requested_feature_dim\": " << opt.featureDim << ",\n"
        << "  \"feature_layout\": \"" << layoutName(opt.layout) << "\",\n"
        << "  \"feature_storage_dtype\": \"" << featureDtypeName(opt.featureDtype) << "\",\n"
        << "  \"quiet_only\": " << (opt.quietOnly ? "true" : "false") << ",\n"
        << "  \"write_labels\": " << (opt.writeLabels ? "true" : "false") << ",\n"
        << "  \"write_sidecars\": " << (opt.writeSidecars ? "true" : "false") << ",\n"
        << "  \"output_format\": \"" << outputFormatName(opt.outputFormat) << "\",\n"
        << "  \"skipped\": " << plan.skipped << "\n"
        << "}\n";
}

void writeNpz(const Options& opt, const Plan& plan) {
    if (!opt.output.parent_path().empty())
        std::filesystem::create_directories(opt.output.parent_path());

    std::vector<char> featuresHeader = npyHeader(featureDescr(opt.featureDtype), {plan.candidates, opt.featureDim});
    std::vector<char> labelsHeader = npyHeader("<f4", {plan.candidates});
    std::vector<char> offsetsHeader = npyHeader("<i8", {int64_t(plan.offsets.size())});
    std::vector<char> targetsHeader = npyHeader("<i8", {int64_t(plan.targets.size())});
    std::vector<char> bucketFlagsHeader = npyHeader("<i8", {int64_t(plan.bucketFlags.size())});

    NpzWriter npz(opt.output);
    writeFeatures(npz, opt, plan, featuresHeader);
    if (opt.writeLabels)
        writeLabels(npz, plan, labelsHeader);
    writeVectorArray(npz, "group_offsets.npy", plan.offsets, offsetsHeader);
    writeVectorArray(npz, "target_indices.npy", plan.targets, targetsHeader);
    writeVectorArray(npz, "bucket_flags.npy", plan.bucketFlags, bucketFlagsHeader);
    npz.finish();

    writeMetaJson(opt.output.string() + ".meta.json", opt, plan);
    if (opt.writeSidecars)
        writeSidecars(opt, plan);
}

void writeNpyDir(const Options& opt, const Plan& plan) {
    std::filesystem::create_directories(opt.output);

    std::vector<char> featuresHeader = npyHeader(featureDescr(opt.featureDtype), {plan.candidates, opt.featureDim});
    std::vector<char> labelsHeader = npyHeader("<f4", {plan.candidates});
    std::vector<char> offsetsHeader = npyHeader("<i8", {int64_t(plan.offsets.size())});
    std::vector<char> targetsHeader = npyHeader("<i8", {int64_t(plan.targets.size())});
    std::vector<char> bucketFlagsHeader = npyHeader("<i8", {int64_t(plan.bucketFlags.size())});

    writeFeaturesNpy(opt.output / "features.npy", opt, plan, featuresHeader);
    if (opt.writeLabels)
        writeLabelsNpy(opt.output / "labels.npy", plan, labelsHeader);
    writeVectorNpy(opt.output / "group_offsets.npy", plan.offsets, offsetsHeader);
    writeVectorNpy(opt.output / "target_indices.npy", plan.targets, targetsHeader);
    writeVectorNpy(opt.output / "bucket_flags.npy", plan.bucketFlags, bucketFlagsHeader);
    writeMetaJson(opt.output / "meta.json", opt, plan);
    if (opt.writeSidecars)
        writeSidecars(opt, plan);
}

void writeOutput(const Options& opt, const Plan& plan) {
    if (opt.outputFormat == OutputFormat::NpyDir)
        writeNpyDir(opt, plan);
    else
        writeNpz(opt, plan);
}

} // namespace

int main(int argc, char** argv) {
    try {
        Options opt = parseOptions(argc, argv);

        BB::init();
        Magics::init();
        Zobrist::init();

        Plan plan = buildPlan(opt);
        writeOutput(opt, plan);

        std::cout << "packed " << plan.positions.size() << " positions, "
                  << plan.candidates << " move candidates to " << opt.output
                  << "; skipped " << plan.skipped << "\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
