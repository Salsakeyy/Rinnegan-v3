#pragma once

#include "position.h"
#include <cstddef>
#include <string>
#include <utility>

namespace NNUE {

constexpr int INPUT = 768;
constexpr int HIDDEN = 768;
constexpr int QA = 181;
constexpr int QB = 64;
constexpr int SCALE = 400;
constexpr int QAB = QA * QB;

struct alignas(64) Accumulator {
    int16_t vals[2][HIDDEN] = {};
};

bool load(const std::string& path);
bool isLoaded();
const std::string& loadedPath();
size_t expectedFileSize();
size_t expectedPaddedFileSize();

void refresh(const Position& pos, Accumulator& acc);
void addPiece(Accumulator& acc, Color c, PieceType pt, Square sq);
void subPiece(Accumulator& acc, Color c, PieceType pt, Square sq);
void movePiece(Accumulator& acc, Color c, PieceType pt, Square from, Square to);

int evaluate(const Accumulator& acc, Color stm);

std::pair<int, int> featureIdx(Color c, PieceType pt, Square sq);

} // namespace NNUE
