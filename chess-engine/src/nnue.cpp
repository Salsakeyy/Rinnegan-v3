#include "nnue.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

#ifdef __AVX2__
#include <immintrin.h>
#endif

namespace NNUE {
namespace {

struct NetData {
    alignas(64) std::array<int16_t, INPUT * HIDDEN> featureWeights{};
    alignas(64) std::array<int16_t, HIDDEN> featureBias{};
    alignas(64) std::array<int16_t, 2 * HIDDEN> outputWeights{};
    int16_t outputBias = 0;
    bool loaded = false;
    std::string path;
};

NetData netData;

constexpr size_t ExpectedInt16Count =
    size_t(INPUT) * HIDDEN + HIDDEN + size_t(2) * HIDDEN + 1;
constexpr size_t ExpectedByteSize = ExpectedInt16Count * sizeof(int16_t);
constexpr size_t ExpectedPaddedByteSize = (ExpectedByteSize + 63) & ~size_t(63);

inline void applyDelta(int16_t* dst, const int16_t* src, int sign) {
#ifdef __AVX2__
    const __m256i deltaSign = _mm256_set1_epi16(int16_t(sign));
    for (int i = 0; i < HIDDEN; i += 16) {
        __m256i cur = _mm256_load_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i row = _mm256_load_si256(reinterpret_cast<const __m256i*>(src + i));
        __m256i upd = _mm256_sign_epi16(row, deltaSign);
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_add_epi16(cur, upd));
    }
#else
    for (int i = 0; i < HIDDEN; ++i)
        dst[i] = int16_t(dst[i] + sign * src[i]);
#endif
}

inline void applyFeature(Accumulator& acc, Color c, PieceType pt, Square sq, int sign) {
    auto [whiteIdx, blackIdx] = featureIdx(c, pt, sq);
    applyDelta(acc.vals[WHITE], netData.featureWeights.data() + size_t(whiteIdx) * HIDDEN, sign);
    applyDelta(acc.vals[BLACK], netData.featureWeights.data() + size_t(blackIdx) * HIDDEN, sign);
}

} // namespace

std::pair<int, int> featureIdx(Color c, PieceType pt, Square sq) {
    int w = (c == WHITE ? 0 : 384) + 64 * int(pt) + int(sq);
    int b = (c == WHITE ? 384 : 0) + 64 * int(pt) + int(sq ^ 56);
    return {w, b};
}

bool load(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        netData.loaded = false;
        netData.path.clear();
        return false;
    }

    std::streamsize size = in.tellg();
    if (size != static_cast<std::streamsize>(ExpectedByteSize) &&
        size != static_cast<std::streamsize>(ExpectedPaddedByteSize)) {
        netData.loaded = false;
        netData.path.clear();
        return false;
    }

    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(netData.featureWeights.data()),
            std::streamsize(netData.featureWeights.size() * sizeof(int16_t)));
    in.read(reinterpret_cast<char*>(netData.featureBias.data()),
            std::streamsize(netData.featureBias.size() * sizeof(int16_t)));
    in.read(reinterpret_cast<char*>(netData.outputWeights.data()),
            std::streamsize(netData.outputWeights.size() * sizeof(int16_t)));
    in.read(reinterpret_cast<char*>(&netData.outputBias), sizeof(netData.outputBias));

    if (!in) {
        netData.loaded = false;
        netData.path.clear();
        return false;
    }

    netData.loaded = true;
    netData.path = path;
    return true;
}

bool isLoaded() {
    return netData.loaded;
}

const std::string& loadedPath() {
    return netData.path;
}

size_t expectedFileSize() {
    return ExpectedByteSize;
}

size_t expectedPaddedFileSize() {
    return ExpectedPaddedByteSize;
}

void refresh(const Position& pos, Accumulator& acc) {
    std::memcpy(acc.vals[WHITE], netData.featureBias.data(), sizeof(acc.vals[WHITE]));
    std::memcpy(acc.vals[BLACK], netData.featureBias.data(), sizeof(acc.vals[BLACK]));

    if (!netData.loaded) return;

    for (int sq = A1; sq <= H8; ++sq) {
        Piece piece = pos.pieceOn(Square(sq));
        if (piece == NO_PIECE) continue;
        addPiece(acc, pieceColor(piece), pieceType(piece), Square(sq));
    }
}

void addPiece(Accumulator& acc, Color c, PieceType pt, Square sq) {
    if (!netData.loaded) return;
    applyFeature(acc, c, pt, sq, +1);
}

void subPiece(Accumulator& acc, Color c, PieceType pt, Square sq) {
    if (!netData.loaded) return;
    applyFeature(acc, c, pt, sq, -1);
}

void movePiece(Accumulator& acc, Color c, PieceType pt, Square from, Square to) {
    if (!netData.loaded) return;
    subPiece(acc, c, pt, from);
    addPiece(acc, c, pt, to);
}

int evaluate(const Accumulator& acc, Color stm) {
    if (!netData.loaded) return 0;

    const int16_t* us = acc.vals[stm];
    const int16_t* them = acc.vals[~stm];

    int64_t sum = 0;
    for (int i = 0; i < HIDDEN; ++i) {
        int32_t u = std::clamp<int>(us[i], 0, QA);
        int32_t t = std::clamp<int>(them[i], 0, QA);

        sum += int64_t(u) * u * netData.outputWeights[i];
        sum += int64_t(t) * t * netData.outputWeights[HIDDEN + i];
    }

    sum /= QA;
    sum += netData.outputBias;
    return int(sum * SCALE / QAB);
}

} // namespace NNUE
