#pragma once

#include "types.h"

namespace Magics {

void init();

Bitboard rookAttacks(Square s, Bitboard occ);
Bitboard bishopAttacks(Square s, Bitboard occ);

} // namespace Magics
