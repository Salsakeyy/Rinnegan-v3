#pragma once

#include "position.h"
#include "types.h"
#include <climits>
#include <cstdint>
#include <cstring>

#if defined(__GNUC__) || defined(__clang__)
#  define TT_PREFETCH(p) __builtin_prefetch((p))
#else
#  include <xmmintrin.h>
#  define TT_PREFETCH(p) _mm_prefetch(reinterpret_cast<const char*>(p), _MM_HINT_T0)
#endif

enum TTFlag : uint8_t {
    TT_NONE  = 0,
    TT_EXACT = 1,
    TT_LOWER = 2, // fail-high (beta cutoff)
    TT_UPPER = 3, // fail-low (alpha not raised)
};

constexpr uint8_t TT_GEN_BITS = 5;
constexpr uint8_t TT_GEN_MASK = (1u << TT_GEN_BITS) - 1; // 0x1F, 32 generations
constexpr uint8_t TT_GEN_CYCLE = 1u << TT_GEN_BITS;
constexpr uint8_t TT_BOUND_SHIFT = TT_GEN_BITS;          // bits 5-6
constexpr uint8_t TT_BOUND_MASK = 0x3u << TT_BOUND_SHIFT;
constexpr uint8_t TT_PV_BIT = 0x80;                      // bit 7

// 10 bytes: key16 | bestMove | score | staticEval | depth | genBoundPv
struct TTEntry {
    uint16_t key16      = 0;
    Move     bestMove   = MOVE_NONE;
    int16_t  score      = 0;
    int16_t  staticEval = SCORE_NONE;
    uint8_t  depth      = 0;
    uint8_t  genBoundPv = 0; // bits 0-4 generation, 5-6 bound, 7 pv

    TTFlag flag()        const { return TTFlag((genBoundPv & TT_BOUND_MASK) >> TT_BOUND_SHIFT); }
    uint8_t generation() const { return genBoundPv & TT_GEN_MASK; }
    bool   isPv()        const { return (genBoundPv & TT_PV_BIT) != 0; }
};
static_assert(sizeof(TTEntry) == 10, "TTEntry must be 10 bytes");

constexpr int TT_CLUSTER_SIZE = 3;

// 32-byte cache-line-friendly cluster: 3 * 10 = 30 bytes + 2 padding.
struct alignas(32) TTCluster {
    TTEntry entries[TT_CLUSTER_SIZE];
    uint8_t padding[2];
};
static_assert(sizeof(TTCluster) == 32, "TTCluster must be 32 bytes");

inline bool ttMoveIsUsable(const Position& pos, Move move) {
    if (!move) return true;
    Piece fromPiece = pos.pieceOn(move.from());
    return fromPiece != NO_PIECE && pieceColor(fromPiece) == pos.sideToMove();
}

class TranspositionTable {
public:
    TranspositionTable() { resize(16); } // 16 MB default

    ~TranspositionTable() { delete[] clusters_; }

    TranspositionTable(const TranspositionTable&) = delete;
    TranspositionTable& operator=(const TranspositionTable&) = delete;

    void resize(int mbSize) {
        size_t bytes = size_t(mbSize) * 1024 * 1024;
        size_t n = bytes / sizeof(TTCluster);
        if (n < 1) n = 1;
        n = floorPowerOfTwo(n);
        delete[] clusters_;
        clusters_ = new TTCluster[n]();
        numClusters_ = n;
        clusterMask_ = n - 1;
        generation_ = 0;
    }

    void clear() {
        std::memset(clusters_, 0, numClusters_ * sizeof(TTCluster));
        generation_ = 0;
    }

    // Bump generation; called once at the start of each `go`.
    void newSearch() {
        generation_ = (generation_ + 1) & TT_GEN_MASK;
    }

    uint8_t generation() const { return generation_; }

    // Issue a non-blocking prefetch for the cluster that `key` maps to.
    void prefetch(uint64_t key) const {
        TT_PREFETCH(&clusters_[index(key)]);
    }

    // Probe the cluster. On hit, returns pointer to matched entry and refreshes
    // its generation so it survives aging. On miss, sets `found=false`; the
    // returned pointer is unspecified — callers must not dereference it.
    TTEntry* probe(uint64_t key, bool& found) {
        TTCluster& cluster = clusters_[index(key)];
        const uint16_t k16 = signature(key);
        for (int i = 0; i < TT_CLUSTER_SIZE; ++i) {
            TTEntry& e = cluster.entries[i];
            if (e.flag() != TT_NONE && e.key16 == k16) {
                // Refresh the generation bits while preserving bound + pv.
                e.genBoundPv = uint8_t(generation_) |
                               uint8_t(e.genBoundPv & (TT_BOUND_MASK | TT_PV_BIT));
                found = true;
                return &e;
            }
        }
        found = false;
        return &cluster.entries[0];
    }

    void store(uint64_t key, int score, TTFlag flag, int depth, Move bestMove, int staticEval) {
        TTCluster& cluster = clusters_[index(key)];
        const uint16_t k16 = signature(key);

        // 1) prefer empty / same-key slot
        TTEntry* slot = nullptr;
        for (int i = 0; i < TT_CLUSTER_SIZE; ++i) {
            TTEntry& e = cluster.entries[i];
            if (e.flag() == TT_NONE || e.key16 == k16) {
                slot = &e;
                break;
            }
        }

        // 2) otherwise pick worst (depth - 8 * relAge)
        if (!slot) {
            int worstQuality = INT_MAX;
            for (int i = 0; i < TT_CLUSTER_SIZE; ++i) {
                TTEntry& e = cluster.entries[i];
                int relAge = (TT_GEN_CYCLE + generation_ - e.generation()) & TT_GEN_MASK;
                int quality = int(e.depth) - 8 * relAge;
                if (quality < worstQuality) {
                    worstQuality = quality;
                    slot = &e;
                }
            }
        }

        const bool sameKey = (slot->key16 == k16 && slot->flag() != TT_NONE);

        // Stockfish-style guard: refuse to overwrite a same-key entry with
        // shallower non-exact info unless it has aged out. Without aging
        // pressure, deeper bounds on the same position dominate.
        if (sameKey && flag != TT_EXACT) {
            int relAge = (TT_GEN_CYCLE + generation_ - slot->generation()) & TT_GEN_MASK;
            if (relAge == 0 && depth + 4 < int(slot->depth))
                return;
        }

        // Preserve a previously stored bestmove when the new store has none.
        Move keepMove = (bestMove == MOVE_NONE && sameKey) ? slot->bestMove : bestMove;

        slot->key16      = k16;
        slot->bestMove   = keepMove;
        slot->score      = int16_t(score);
        slot->staticEval = int16_t(staticEval);
        slot->depth      = uint8_t(depth);
        slot->genBoundPv = uint8_t(generation_) |
                           uint8_t(uint8_t(flag) << TT_BOUND_SHIFT);
    }

private:
    size_t index(uint64_t key) const {
        return size_t(key) & clusterMask_;
    }

    static size_t floorPowerOfTwo(size_t n) {
        size_t p = 1;
        while ((p << 1) != 0 && (p << 1) <= n)
            p <<= 1;
        return p;
    }

    // Use upper bits for the signature so it is independent of the cluster
    // index (which uses the low bits via modulo numClusters_).
    static uint16_t signature(uint64_t key) {
        return uint16_t(key >> 48);
    }

    TTCluster* clusters_ = nullptr;
    size_t numClusters_ = 0;
    size_t clusterMask_ = 0;
    uint8_t generation_ = 0;
};
