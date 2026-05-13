#pragma once

#include "position.h"
#include <string>

namespace Eval {

void init();
int evaluateClassical(const Position& pos);
int evaluate(const Position& pos);
std::string trace(const Position& pos);

} // namespace Eval
