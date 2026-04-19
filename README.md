# Rinnegan

A UCI chess engine written in C++20. Bitboard-based with magic bitboards for
sliding pieces, tapered PeSTO evaluation, and a modern search stack (PVS, LMR,
null move, TT, killers/history/counter-moves, futility/LMP/RFP/razoring).

## Versions

| Version | Approx. Elo (TC) | Notes |
|---------|------------------|-------|
| v1      | ~1900            | Baseline: bitboard movegen, alpha-beta, TT, qsearch |
| v2      | ~2100 (10+0.1)   | +172 Elo vs v1 (SPRT). Added PVS + LMR, fixed FEN parsing |
| **v3**  | **~2500+ (10+0.1)** | **+438 Elo vs v2 (SPRT, 162 games, LOS 100%, 141W / 3L / 18D)** |
| v4      | target ~2800+    | Lazy SMP + NNUE integration, pending SPRT |

v2's 2089 Elo was anchored vs Stockfish (UCI_Elo 1320–2500, 3+0.05, 150 rounds/anchor,
combined inverse-variance-weighted estimate). v3 is +438 Elo over that baseline at
a slightly longer TC, putting it well into master strength (≥2300).

---

## v3 changelog

### Evaluation (`src/eval.cpp`)

Kept v2's tapered PeSTO material + piece-square tables, added structural and
dynamic terms:

- **Bishop pair** — MG +30 / EG +50 when a side has ≥2 bishops.
- **Passed pawns** — tapered bonus by relative rank (pawn's side perspective).
  Detected via precomputed `PassedPawnMask[color][sq]`: no enemy pawn on the
  pawn's file or adjacent files, anywhere ahead of it.
- **Doubled pawns** — `-10` MG / `-20` EG per extra pawn stacked on a file.
- **Isolated pawns** — `-12` MG / `-18` EG per pawn with no friendly pawn on
  adjacent files.
- **Rooks on (semi-)open files** — +25 MG / +10 EG on open, +15 / +5 on
  semi-open (no own pawn on the file).
- **Mobility** — for N/B/R/Q, count attacked squares that aren't blocked by
  own pieces and aren't attacked by enemy pawns; scaled by piece type.
- **King safety** — count weighted attackers (N=2, B=2, R=3, Q=5) landing on
  the enemy king zone (king + neighbours + one rank forward). Apply a
  quadratic penalty `units² / 4`, capped at 500cp, only when ≥2 distinct
  attacking piece-types are present. This avoids single-piece false alarms
  while still catching real attacks.
- **Pawn shield** — +12 MG per friendly pawn on the three files in front of
  the king, on the two ranks nearest to it.

A new `Eval::init()` precomputes the passed-pawn, isolated, king-zone and
pawn-shield masks once at startup; it's wired from `main.cpp`.

### Search (`src/search.cpp`)

- **Mate distance pruning** — prune trivially at the root of each node when a
  faster mate has already been found up the tree.
- **Reverse Futility Pruning (static null move)** — at `depth ≤ 6` on non-PV
  nodes, if `staticEval − 80·depth ≥ beta`, return `staticEval`.
- **Razoring** — at `depth ≤ 2`, if `staticEval + 300 < alpha`, drop straight
  to a null-window quiescence search.
- **Frontier futility pruning** — skip quiet moves at `depth ≤ 3` when
  `staticEval + 90 + 80·depth ≤ alpha`.
- **Late Move Pruning** — skip quiet moves at `depth ≤ 4` once `move_number >
  3 + depth²`.
- **Log-based LMR table** — replaced v2's ad-hoc formula with
  `lmrTable[d][m] = floor(0.75 + log(d)·log(m)/2.25)`, built once at startup.
  Reduced by 1 on PV nodes.
- **History gravity** — on a beta cutoff, a depth² bonus goes to the cutoff
  move and a matching malus goes to every earlier quiet move that failed to
  cut. The gravity formula `entry += bonus − entry·|bonus|/HISTORY_MAX` keeps
  the table self-normalizing.
- **Counter-move heuristic** — `counter[color][prevFrom][prevTo]` stores the
  reply that produced a cutoff last time this move was played; it gets an
  ordering score between killers and history.
- **TT static-eval reuse** — reuse the stored `staticEval` across TT hits
  instead of calling `Eval::evaluate()` again; v3 eval is more expensive so
  this matters.
- **Soft/hard time split** — iterative deepening breaks on a soft limit; the
  hard limit (capped at `timeLeft/3`) lets an in-flight iteration finish.
  The soft limit gently shrinks once the best move has been stable for 3+
  iterations.

### UCI / init

- `id name` now reports `Rinnegan v3`.
- `main.cpp` calls `Eval::init()` and `Search::init()` at startup.

---

## v4 changelog

### Search / threading (`src/search.cpp`, `src/thread.h`)

- Added Lazy SMP with a shared TT and per-thread `ThreadData`.
- Each worker owns its own `Position`, `StateInfo` stack, move-ordering tables,
  and NNUE accumulator stack.
- Thread `0` remains the main worker for UCI `info` output and wall-clock time
  checks.
- TT replacement now prefers exact scores slightly harder under SMP races, and
  probed TT moves are guarded before use.

### NNUE (`src/nnue.h`, `src/nnue.cpp`)

- Added support for Bullet / Akimbo-style `(768 -> 768) x 2 -> 1` SCReLU nets.
- Loader expects raw int16 weights on disk and falls back to PeSTO if loading
  fails or the net size is wrong.
- Search keeps a per-ply accumulator stack and updates it incrementally across
  quiet moves, captures, en passant, castling, and promotions.

### Eval / UCI / tooling

- `Eval::evaluate(pos, acc)` now dispatches to NNUE when a net is loaded and
  `UseNNUE=true`, otherwise it preserves the v3 classical evaluator.
- New UCI options: `Threads`, `EvalFile`, and `UseNNUE`.
- Added `bench` to search 16 fixed positions at depth 13 and report aggregate
  nodes / NPS.
- Added `tests/sprt_v4_vs_v3.sh` for the v4-vs-v3 SPRT gate.

---

## How the engine is structured

```
src/
  types.h          Move encoding (16-bit), Piece/Color/Square enums, State
  bitboard.[h|cpp] Precomputed pawn/knight/king attack tables, popcount helpers
  magics.[h|cpp]   Magic bitboards for bishop/rook attack lookup
  zobrist.[h|cpp]  Zobrist keys for TT indexing
  position.[h|cpp] Board representation, make/unmake, legality, draw detection
  movegen.[h|cpp]  Pseudo-legal + legal move generation, captures-only for qs
  eval.[h|cpp]     Tapered PeSTO + v3 structural/king-safety/mobility terms
  tt.[h]           Transposition table with key32/score/staticEval/bestMove
  search.[h|cpp]   Iterative deepening, negamax + PVS + LMR + all v3 pruning
  uci.[h|cpp]      UCI protocol loop
  main.cpp         init + UCI loop
```

## Build & run

```
cmake -B build
cmake --build build -j
./build/engine        # UCI loop
./build/perft         # move-gen correctness (6 standard positions)
```

Default NNUE path is `./rinnegan-v4.net`. If the net lives elsewhere, point the
engine at it with `setoption name EvalFile value /path/to/net`.

## Testing

- `tests/sprt_v2_vs_v1.sh`, `tests/sprt_v3_vs_v2.sh`,
  `tests/sprt_v4_vs_v3.sh` — SPRT matches via `cutechess-cli` or `fastchess`.
- `tests/gauntlet_elo.sh` — absolute Elo estimate by playing vs Stockfish at
  several `UCI_Elo` anchors (`anchors.conf`). Uses inverse-variance weighting
  to combine anchors. Requires `stockfish` on PATH.
- `tests/openings.epd` — 40-position opening book used by both harnesses.

## Results

v3 vs v2 SPRT (10+0.1, 16MB, 1 thread, openings.epd):

```
Elo: 438.76 +/- 80.80, nElo: 755.08 +/- 53.50
LOS: 100.00 %, DrawRatio: 4.94 %
Games: 162, Wins: 141, Losses: 3, Draws: 18
LLR: 2.98 (101.1%) (-2.94, 2.94) [0.00, 10.00]
SPRT ([0.00, 10.00]) completed - H1 was accepted
```
