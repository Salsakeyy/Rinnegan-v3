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
    // Number of per-move bonus computations whose pre-clamp magnitude
    // was >= the active clamp limit. High saturation means the clamp is
    // collapsing distinguishable model outputs to a single ceiling, so
    // within-bucket ordering falls back to history. Used by the
    // rootset bench's clamp sweep (see docs/policy_v2_plan.md).
    uint64_t saturatedBonuses = 0;
    // Total per-move bonus computations attributed to the v2 path. The
    // ratio `saturatedBonuses / scoredMoves` is the saturation rate.
    uint64_t scoredMoves = 0;
};
PerfCounters readPerfCounters();
void resetPerfCounters();

} // namespace Policy
