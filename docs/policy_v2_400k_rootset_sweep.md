# Policy v2 400k Root-Set Sweep

This is a compact sweep over the new 500-position root-set bench. It uses the
same sampled FENs as the earlier candidate checks, so the rows are comparable.

No retraining, exporter changes, or architecture changes were made.

## Sweep Setup

- Engine: `chess-engine/build/engine`
- Policy file: `data/policy/v2/run-stockfish-mpv8-d14d15-400k-h256/policy_eg10.bin`
- FEN source: `data/policy/policy-10m-fast/dataset.fens.txt`
- Sample: 500 roots, seed `20260511`
- Search: fixed depth 8
- Threads / hash: 1 / 16 MB
- Modes:
  - root_bonus: `PolicyV2Mode=1`
  - quiet_residual: `PolicyV2Mode=2`
- Scales: `100, 200, 400, 600, 800, 1000, 1200, 1500`
- Endgame scale: `0, 100`

Command:

```bash
scales=(100 200 400 600 800 1000 1200 1500); cfgs=(); for mode in 1 2; do for scale in "${scales[@]}"; do for eg in 0 100; do if [[ "$mode" == 1 ]]; then label="400k_rb_s${scale}_eg${eg}"; else label="400k_qr_s${scale}_eg${eg}"; fi; cfgs+=("${label}:data/policy/v2/run-stockfish-mpv8-d14d15-400k-h256/policy_eg10.bin:${mode}:${scale}:${scale}:${eg}"); done; done; done; CONFIGS="$(IFS=,; echo "${cfgs[*]}")" SAMPLE_SIZE=500 DEPTH=8 OUT_DIR=data/policy/v2/rootset/400k_compact_sweep_d8_500 ./scripts/bench_policy_v2_rootset.sh
```

Outputs:

- Root JSONL: `chess-engine/data/policy/v2/rootset/400k_compact_sweep_d8_500/rootset.jsonl`
- Root CSV: `chess-engine/data/policy/v2/rootset/400k_compact_sweep_d8_500/rootset.csv`
- Root summary: `chess-engine/data/policy/v2/rootset/400k_compact_sweep_d8_500/rootset_summary.md`

## Root-Set Screen

Best rows by node ratio were almost inert:

| config | changes | change rate | node ratio | wall ratio |
|---|---:|---:|---:|---:|
| `400k_rb_s1000_eg0` | 2 | 0.40% | 0.9986 | 1.0388 |
| `400k_rb_s1000_eg100` | 2 | 0.40% | 0.9986 | 1.0801 |
| `400k_rb_s1200_eg0` | 2 | 0.40% | 0.9986 | 1.0727 |
| `400k_rb_s1500_eg100` | 2 | 0.40% | 0.9986 | 1.0108 |
| `400k_qr_s1000_eg100` | 0 | 0.00% | 1.0000 | 1.0020 |

Meaningful-change rows were all node-negative:

| config | changes | change rate | node ratio | wall ratio |
|---|---:|---:|---:|---:|
| `400k_qr_s100_eg0` | 75 | 15.00% | 1.0151 | 1.0345 |
| `400k_qr_s200_eg100` | 107 | 21.40% | 1.0179 | 1.0237 |
| `400k_rb_s200_eg100` | 106 | 21.20% | 1.0185 | 1.0425 |
| `400k_qr_s400_eg0` | 81 | 16.20% | 1.0215 | 1.0269 |
| `400k_rb_s400_eg0` | 82 | 16.40% | 1.0242 | 1.0222 |
| `400k_rb_s400_eg100` | 115 | 23.00% | 1.0300 | 1.0314 |

The pattern is clear: when the policy is strong enough to perturb root choices,
it increases searched nodes. When it reaches neutral/slightly lower nodes, it
barely changes anything.

## Stockfish Adjudication

I adjudicated the six rows with meaningful root movement and tolerable wall
ratio. Stockfish depth was 12, one thread, 64 MB hash.

Command:

```bash
python3 tools/policy/v2/rootset_sf_adjudicate.py --input data/policy/v2/rootset/400k_compact_sweep_d8_500/rootset.jsonl --stockfish /opt/homebrew/bin/stockfish --max-changed-per-config 150 --depth 12 --threads 1 --hash-mb 64 --out-jsonl data/policy/v2/rootset/400k_compact_sweep_d8_500/sf_adjudication_top/adjudication.jsonl --out-csv data/policy/v2/rootset/400k_compact_sweep_d8_500/sf_adjudication_top/adjudication.csv --summary-md data/policy/v2/rootset/400k_compact_sweep_d8_500/sf_adjudication_top/adjudication_summary.md --config-label 400k_qr_s100_eg0 --config-label 400k_qr_s200_eg100 --config-label 400k_rb_s200_eg100 --config-label 400k_qr_s400_eg0 --config-label 400k_rb_s400_eg0 --config-label 400k_rb_s400_eg100
```

Outputs:

- Adjudication JSONL: `chess-engine/data/policy/v2/rootset/400k_compact_sweep_d8_500/sf_adjudication_top/adjudication.jsonl`
- Adjudication CSV: `chess-engine/data/policy/v2/rootset/400k_compact_sweep_d8_500/sf_adjudication_top/adjudication.csv`
- Adjudication summary: `chess-engine/data/policy/v2/rootset/400k_compact_sweep_d8_500/sf_adjudication_top/adjudication_summary.md`

| config | changed roots adjudicated | SF prefers candidate | avg cp gain |
|---|---:|---:|---:|
| `400k_qr_s100_eg0` | 75 | 53.33% | -1.33 |
| `400k_qr_s200_eg100` | 107 | 50.47% | +2.66 |
| `400k_rb_s200_eg100` | 106 | 50.00% | +2.30 |
| `400k_qr_s400_eg0` | 81 | 49.38% | +1.05 |
| `400k_rb_s400_eg0` | 82 | 50.00% | +1.43 |
| `400k_rb_s400_eg100` | 115 | 48.70% | +3.31 |

Stockfish does not show a real quality edge. The preference rates are clustered
around 50%, and average cp gains are tiny. This means the extra searched nodes
and wall time are not buying systematically better root moves.

## Conclusion

No swept config passes the pre-SPRT screen.

- Node-positive configs are inert: 0 to 2 root changes over 500 positions.
- Root-active configs are node-negative by about 1.5% to 3.0%.
- Root-active configs are wall-negative by about 2.2% to 4.3% in the best cases.
- Stockfish adjudication is neutral on changed roots.

If a mini-match must be run anyway, the least-bad screen candidate is
`400k_qr_s200_eg100`: it has the lowest wall penalty among the active configs
and a tiny positive Stockfish cp average. It is still not a full-SPRT candidate.

Recommendation: do not spend SPRT time on this 400k policy family as-is. The
next useful step is more/better data before another candidate cycle.
