# Policy v2 Better Bench

This adds a broader pre-SPRT evaluation stack for v2 move-ordering policies.
It does not retrain, change the model, or change the exporter format. The old
16-position engine `bench` remains useful as a smoke/signature test, but it is
no longer the main diagnostic for policy configs.

## New Tools

- `chess-engine/tools/policy/v2/rootset_bench.py`
  - Runs policy off and one or more v2 candidates on a sampled root set.
  - Writes per-position JSONL and CSV.
  - Records bestmove, nodes, nps, wall time, phase bucket, chosen move class,
    baseline delta, policy root ordering, raw model score, scaled/clamped bonus,
    and policy latency counters.
- `chess-engine/tools/policy/v2/rootset_sf_adjudicate.py`
  - Reads root-set output and sends only changed root moves to Stockfish.
  - Records Stockfish best move, baseline move eval, candidate move eval,
    gap to Stockfish best, and candidate cp gain/loss.
- `scripts/bench_policy_v2_rootset.sh`
  - Thin wrapper around the root-set bench.
- `scripts/adjudicate_policy_v2_with_stockfish.sh`
  - Thin wrapper around the Stockfish adjudicator.
- `scripts/mini_match_policy_v2.sh`
  - Short paired gauntlet wrapper around the existing fastchess harness.

The existing SPRT harness now also accepts `QUIET_SCALE` for v2 configs. The
default remains `QUIET_SCALE=$SCALE`, so existing calls keep their behavior.

## Usage

Root-set bench:

```bash
CONFIGS='400k_qr_s400_eg0:data/policy/v2/run-stockfish-mpv8-d14d15-400k-h256/policy_eg10.bin:2:400:400:0' \
SAMPLE_SIZE=500 \
DEPTH=8 \
OUT_DIR=data/policy/v2/rootset/400k_qr_s400_eg0 \
./scripts/bench_policy_v2_rootset.sh
```

Multiple configs can be passed as a comma-separated list:

```bash
CONFIGS='label_a:path/to/policy.bin:1:600:600:100,label_b:path/to/policy.bin:2:400:400:0' \
./scripts/bench_policy_v2_rootset.sh
```

Stockfish adjudication:

```bash
DEPTH=12 \
IN_DIR=data/policy/v2/rootset/400k_qr_s400_eg0 \
OUT_DIR=data/policy/v2/rootset/400k_qr_s400_eg0/sf_adjudication \
./scripts/adjudicate_policy_v2_with_stockfish.sh
```

Mini-match screen:

```bash
MODE=2 \
SCALE=400 \
QUIET_SCALE=400 \
ENDGAME_SCALE=0 \
ROUNDS=50 \
TC='10+0.1' \
./scripts/mini_match_policy_v2.sh
```

## Example Run

Candidate:

- Policy file: `data/policy/v2/run-stockfish-mpv8-d14d15-400k-h256/policy_eg10.bin`
- `UsePolicyV2=true`
- `PolicyV2Mode=2`
- `PolicyV2Scale=400`
- `PolicyV2QuietScale=400`
- `PolicyV2EndgameScale=0`

Root-set command used:

```bash
SAMPLE_SIZE=500 DEPTH=8 OUT_DIR=data/policy/v2/rootset/400k_qr_s400_eg0 CONFIGS='400k_qr_s400_eg0:data/policy/v2/run-stockfish-mpv8-d14d15-400k-h256/policy_eg10.bin:2:400:400:0' ./scripts/bench_policy_v2_rootset.sh
```

Stockfish adjudication command used:

```bash
DEPTH=12 IN_DIR=data/policy/v2/rootset/400k_qr_s400_eg0 OUT_DIR=data/policy/v2/rootset/400k_qr_s400_eg0/sf_adjudication ./scripts/adjudicate_policy_v2_with_stockfish.sh
```

Outputs:

- Root JSONL: `chess-engine/data/policy/v2/rootset/400k_qr_s400_eg0/rootset.jsonl`
- Root CSV: `chess-engine/data/policy/v2/rootset/400k_qr_s400_eg0/rootset.csv`
- Root summary: `chess-engine/data/policy/v2/rootset/400k_qr_s400_eg0/rootset_summary.md`
- Adjudication JSONL: `chess-engine/data/policy/v2/rootset/400k_qr_s400_eg0/sf_adjudication/adjudication.jsonl`
- Adjudication CSV: `chess-engine/data/policy/v2/rootset/400k_qr_s400_eg0/sf_adjudication/adjudication.csv`
- Adjudication summary: `chess-engine/data/policy/v2/rootset/400k_qr_s400_eg0/sf_adjudication/adjudication_summary.md`

## Root-Set Result

500 sampled roots, depth 8, one thread, 16 MB hash.

| config | positions | root changes | change rate | node ratio vs off | nps ratio vs off | wall ratio vs off |
|---|---:|---:|---:|---:|---:|---:|
| `400k_qr_s400_eg0` | 500 | 81 | 16.20% | 1.0215 | 0.9390 | 1.0879 |

Phase split:

| phase | positions | changes | change rate | node ratio | wall ratio |
|---|---:|---:|---:|---:|---:|
| opening | 217 | 43 | 19.82% | 1.0088 | 1.0177 |
| middlegame | 160 | 38 | 23.75% | 1.0572 | 1.0553 |
| endgame | 123 | 0 | 0.00% | 1.0000 | 1.3936 |

Candidate move class split:

| candidate move class | positions | changes | change rate |
|---|---:|---:|---:|
| quiet | 330 | 72 | 21.82% |
| capture | 117 | 6 | 5.13% |
| check | 49 | 3 | 6.12% |
| promotion | 4 | 0 | 0.00% |

The candidate clearly changes root decisions at scale, but it does not reduce
nodes on this broader set. It searched 2.15% more nodes and was 8.79% slower
wall-clock than policy off at this depth. Endgame scale 0 prevents endgame root
move changes, but still pays policy overhead on endgame roots.

## Stockfish Adjudication Result

81 changed roots, Stockfish depth 12, one thread, 64 MB hash.

| config | adjudicated | SF prefers candidate | avg cp gain | avg baseline gap | avg candidate gap |
|---|---:|---:|---:|---:|---:|
| `400k_qr_s400_eg0` | 81 | 49.38% | +1.05 cp | 426.15 | 425.10 |

Phase split:

| phase | count | SF prefers candidate | avg cp gain |
|---|---:|---:|---:|
| opening | 43 | 48.84% | -5.37 cp |
| middlegame | 38 | 50.00% | +8.32 cp |

Candidate move-class split:

| candidate move class | count | SF prefers candidate | avg cp gain |
|---|---:|---:|---:|
| quiet | 72 | 50.00% | +4.61 cp |
| capture | 6 | 33.33% | -27.00 cp |
| check | 3 | 66.67% | -28.33 cp |

Baseline policy-rank bucket split:

| baseline move policy-rank bucket | count | SF prefers candidate | avg cp gain |
|---|---:|---:|---:|
| 1 | 13 | 38.46% | -6.85 cp |
| 2-3 | 13 | 46.15% | -22.62 cp |
| 4-5 | 12 | 66.67% | +31.58 cp |
| 6-10 | 8 | 37.50% | -23.00 cp |
| >10 | 35 | 51.43% | +7.80 cp |

Stockfish is essentially split on the changed moves. The average +1.05 cp
delta is too small to matter, especially next to the slower root-set result.
The only mildly positive signal is quiet changed moves in the middlegame, but
it is not strong enough to justify a full SPRT by itself.

## Recommendation

This candidate should not go straight to full SPRT. It answers the three
pre-SPRT questions as follows:

1. Does it change root decisions at scale? Yes: 81/500 roots, 16.20%.
2. Are changed moves better by Stockfish? Not convincingly: 49.38% preferred,
   +1.05 cp average.
3. Does it deserve a short mini-match and then SPRT? Mini-match only if we want
   one game-transfer sanity check for the harness; no full SPRT from this
   evidence.

## Second Candidate Check

I also ran the same pipeline on the other phase-tuning finalist:

- Policy file: `data/policy/v2/run-stockfish-mpv8-d14d15-400k-h256/policy_eg10.bin`
- `UsePolicyV2=true`
- `PolicyV2Mode=1`
- `PolicyV2Scale=600`
- `PolicyV2QuietScale=600`
- `PolicyV2EndgameScale=100`

Root-set command used:

```bash
SAMPLE_SIZE=500 DEPTH=8 OUT_DIR=data/policy/v2/rootset/400k_root_bonus_s600_eg100 CONFIGS='400k_root_bonus_s600_eg100:data/policy/v2/run-stockfish-mpv8-d14d15-400k-h256/policy_eg10.bin:1:600:600:100' ./scripts/bench_policy_v2_rootset.sh
```

Stockfish adjudication command used:

```bash
DEPTH=12 IN_DIR=data/policy/v2/rootset/400k_root_bonus_s600_eg100 OUT_DIR=data/policy/v2/rootset/400k_root_bonus_s600_eg100/sf_adjudication ./scripts/adjudicate_policy_v2_with_stockfish.sh
```

Outputs:

- Root JSONL: `chess-engine/data/policy/v2/rootset/400k_root_bonus_s600_eg100/rootset.jsonl`
- Root CSV: `chess-engine/data/policy/v2/rootset/400k_root_bonus_s600_eg100/rootset.csv`
- Root summary: `chess-engine/data/policy/v2/rootset/400k_root_bonus_s600_eg100/rootset_summary.md`
- Adjudication JSONL: `chess-engine/data/policy/v2/rootset/400k_root_bonus_s600_eg100/sf_adjudication/adjudication.jsonl`
- Adjudication CSV: `chess-engine/data/policy/v2/rootset/400k_root_bonus_s600_eg100/sf_adjudication/adjudication.csv`
- Adjudication summary: `chess-engine/data/policy/v2/rootset/400k_root_bonus_s600_eg100/sf_adjudication/adjudication_summary.md`

500 sampled roots, depth 8, one thread, 16 MB hash.

| config | positions | root changes | change rate | node ratio vs off | nps ratio vs off | wall ratio vs off |
|---|---:|---:|---:|---:|---:|---:|
| `400k_root_bonus_s600_eg100` | 500 | 9 | 1.80% | 1.0024 | 0.9820 | 1.0208 |

Phase split:

| phase | positions | changes | change rate | node ratio | wall ratio |
|---|---:|---:|---:|---:|---:|
| opening | 217 | 1 | 0.46% | 1.0003 | 1.0201 |
| middlegame | 160 | 1 | 0.62% | 0.9946 | 1.0281 |
| endgame | 123 | 7 | 5.69% | 1.0240 | 1.0092 |

Stockfish depth 12 adjudication covered all 9 changed roots.

| config | adjudicated | SF prefers candidate | avg cp gain | avg baseline gap | avg candidate gap |
|---|---:|---:|---:|---:|---:|
| `400k_root_bonus_s600_eg100` | 9 | 11.11% | -10.89 cp | 7.22 | 18.11 |

Phase split:

| phase | count | SF prefers candidate | avg cp gain |
|---|---:|---:|---:|
| opening | 1 | 100.00% | +101.00 cp |
| middlegame | 1 | 0.00% | -16.00 cp |
| endgame | 7 | 0.00% | -26.14 cp |

This root-bonus config is also not a full-SPRT candidate. It is closer to
neutral on nodes than the quiet-residual candidate, but it changes too few root
moves and the changed moves are bad by Stockfish, especially in endgames.

## 200k Reference Check

I also ran the old 200k best/reference candidate through the same pipeline:

- Policy file: `data/policy/v2/run-stockfish-mpv8-d14-200k-h256/policy_eg10.bin`
- `UsePolicyV2=true`
- `PolicyV2Mode=1`
- `PolicyV2Scale=1000`
- `PolicyV2QuietScale=1000`
- `PolicyV2EndgameScale=100`

Root-set command used:

```bash
SAMPLE_SIZE=500 DEPTH=8 OUT_DIR=data/policy/v2/rootset/200k_root_bonus_s1000_eg100 CONFIGS='200k_root_bonus_s1000_eg100:data/policy/v2/run-stockfish-mpv8-d14-200k-h256/policy_eg10.bin:1:1000:1000:100' ./scripts/bench_policy_v2_rootset.sh
```

Stockfish adjudication command used:

```bash
DEPTH=12 IN_DIR=data/policy/v2/rootset/200k_root_bonus_s1000_eg100 OUT_DIR=data/policy/v2/rootset/200k_root_bonus_s1000_eg100/sf_adjudication ./scripts/adjudicate_policy_v2_with_stockfish.sh
```

Outputs:

- Root JSONL: `chess-engine/data/policy/v2/rootset/200k_root_bonus_s1000_eg100/rootset.jsonl`
- Root CSV: `chess-engine/data/policy/v2/rootset/200k_root_bonus_s1000_eg100/rootset.csv`
- Root summary: `chess-engine/data/policy/v2/rootset/200k_root_bonus_s1000_eg100/rootset_summary.md`
- Adjudication JSONL: `chess-engine/data/policy/v2/rootset/200k_root_bonus_s1000_eg100/sf_adjudication/adjudication.jsonl`
- Adjudication CSV: `chess-engine/data/policy/v2/rootset/200k_root_bonus_s1000_eg100/sf_adjudication/adjudication.csv`
- Adjudication summary: `chess-engine/data/policy/v2/rootset/200k_root_bonus_s1000_eg100/sf_adjudication/adjudication_summary.md`

500 sampled roots, depth 8, one thread, 16 MB hash.

| config | positions | root changes | change rate | node ratio vs off | nps ratio vs off | wall ratio vs off |
|---|---:|---:|---:|---:|---:|---:|
| `200k_root_bonus_s1000_eg100` | 500 | 2 | 0.40% | 0.9986 | 0.9718 | 1.0276 |

Phase split:

| phase | positions | changes | change rate | node ratio | wall ratio |
|---|---:|---:|---:|---:|---:|
| opening | 217 | 1 | 0.46% | 1.0003 | 1.0216 |
| middlegame | 160 | 1 | 0.62% | 0.9946 | 1.0337 |
| endgame | 123 | 0 | 0.00% | 1.0000 | 1.0367 |

Stockfish depth 12 adjudication covered both changed roots.

| config | adjudicated | SF prefers candidate | avg cp gain | avg baseline gap | avg candidate gap |
|---|---:|---:|---:|---:|---:|
| `200k_root_bonus_s1000_eg100` | 2 | 50.00% | +42.50 cp | 47.00 | 4.50 |

The 200k reference is not better in a useful way. It is slightly below baseline
on nodes, but slower on wall-clock and changes almost no root decisions. The
Stockfish result is based on only two changed positions, so the positive
average cp gain is not a robust signal.

## Final Recommendation

Single next mini-match candidate:

- `400k_qr_s400_eg0`, with low priority, because it is the only config measured
  through the new stack that changes roots at a meaningful rate. Treat the
  mini-match as a screen, not as a prelude to an assumed SPRT.

Single full SPRT candidate:

- None from these runs. Quiet-residual fails on node/wall efficiency and is
  neutral by Stockfish on changed moves. Root-bonus barely changes roots and is
  worse by Stockfish when it does. The 200k reference is less disruptive, but
  mostly inert and still wall-clock negative. A full SPRT should wait for a
  config that shows at least neutral wall time and a clearer Stockfish
  preference on changed roots, or that wins a mini-match despite this
  diagnostic.
