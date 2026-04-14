#pragma once

#include "types.h"
#include <cstdint>
#include <cstring>

enum TTFlag : uint8_t {
    TT_NONE  = 0,
    TT_EXACT = 1,
    TT_LOWER = 2, // fail-high (beta cutoff)
    TT_UPPER = 3, // fail-low (alpha not raised)
};

struct TTEntry {
    uint32_t key32 = 0;
    int16_t score = 0;
    int16_t staticEval = 0;
    Move bestMove = MOVE_NONE;
    uint8_t depth = 0;
    uint8_t flag = TT_NONE;
};

class TranspositionTable {
public:
    TranspositionTable() { resize(16); } // 16 MB default

    void resize(int mbSize) {
        size_t bytes = size_t(mbSize) * 1024 * 1024;
        numEntries = bytes / sizeof(TTEntry);
        if (numEntries < 1) numEntries = 1;
        delete[] table;
        table = new TTEntry[numEntries]();
    }

    void clear() {
        std::memset(table, 0, numEntries * sizeof(TTEntry));
    }

    TTEntry* probe(uint64_t key, bool& found) {
        TTEntry* entry = &table[key % numEntries];
        found = (entry->key32 == uint32_t(key >> 32) && entry->flag != TT_NONE);
        return entry;
    }

    void store(uint64_t key, int score, TTFlag flag, int depth, Move bestMove, int staticEval) {
        TTEntry* entry = &table[key % numEntries];
        // Always replace if new depth >= stored depth or different position
        if (entry->key32 != uint32_t(key >> 32) || depth >= entry->depth || flag == TT_EXACT) {
            entry->key32 = uint32_t(key >> 32);
            entry->score = int16_t(score);
            entry->staticEval = int16_t(staticEval);
            entry->bestMove = bestMove;
            entry->depth = uint8_t(depth);
            entry->flag = uint8_t(flag);
        }
    }

    ~TranspositionTable() { delete[] table; }

    // No copy
    TranspositionTable(const TranspositionTable&) = delete;
    TranspositionTable& operator=(const TranspositionTable&) = delete;

private:
    TTEntry* table = nullptr;
    size_t numEntries = 0;
};
