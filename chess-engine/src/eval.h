#pragma once

#include "position.h"

namespace NNUE {
struct Accumulator;
}

namespace Eval {

void init();
int evaluateClassical(const Position& pos);
int evaluate(const Position& pos);
int evaluate(const Position& pos, const NNUE::Accumulator* acc);

} // namespace Eval
