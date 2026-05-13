#pragma once

#include "position.h"
#include <cstdint>
#include <string>

namespace Policy {

// v1 (RINPOL1) — kept for A/B and CCRL.
bool load(const std::string& path);
bool isLoaded();
const std::string& loadedPath();
int scoreMove(Position& pos, Move move, int scale);
void scoreMoves(Position& pos, const Move* moves, int count, int scale, int* outBonuses);

// v2 (RINPOL2) — see tools/policy/v2/export_v2.py for the binary layout.
// Adds bucket calibration (per move-class scale + bias) on top of v1.
bool loadV2(const std::string& path);
bool isV2Loaded();
const std::string& loadedV2Path();
void scoreMovesV2(Position& pos, const Move* moves, int count,
                  int scale, int quietScale, int endgameScale,
                  int bonusClamp, int* outBonuses);

// Latency counters. Reset and read from the engine bench harness.
struct PerfCounters {
    uint64_t featureNanos = 0;
    uint64_t forwardNanos = 0;
    uint64_t calls = 0;
};
PerfCounters readPerfCounters();
void resetPerfCounters();

} // namespace Policy
