#pragma once

#include "position.h"
#include <cstdint>

namespace Perft {

uint64_t perft(Position& pos, int depth);
void divide(Position& pos, int depth);

} // namespace Perft
