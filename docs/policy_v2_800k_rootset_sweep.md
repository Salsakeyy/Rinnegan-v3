# Policy v2 800k Root-Set Sweep

This reruns the 500-position depth-8 root-set bench on the new 800k v2 policy.
It uses the same sample seed and search setup as the 400k sweep, with two 400k
reference rows included in the same run.

## Setup

- Engine: `chess-engine/build/engine`
- 800k policy: `data/policy/v2/run-stockfish-mpv8-d14d15d10-800k-h256/policy_eg10.bin`
- 400k reference: `data/policy/v2/run-stockfish-mpv8-d14d15-400k-h256/policy_eg10.bin`
- FEN source: `data/policy/policy-10m-fast/dataset.fens.txt`
- Sample: 500 roots, seed `20260511`
- Search: fixed depth 8
- Threads / hash: 1 / 16 MB
- Modes: `root_bonus` (`PolicyV2Mode=1`) and `quiet_residual` (`PolicyV2Mode=2`)
- Scales: `100, 200, 400, 600, 800, 1000, 1200, 1500`
- Endgame scale: `0, 100`

Outputs:

- Root JSONL: `chess-engine/data/policy/v2/rootset/800k_compact_sweep_d8_500/rootset.jsonl`
- Root CSV: `chess-engine/data/policy/v2/rootset/800k_compact_sweep_d8_500/rootset.csv`
- Root summary: `chess-engine/data/policy/v2/rootset/800k_compact_sweep_d8_500/rootset_summary.md`
- SF adjudication: `chess-engine/data/policy/v2/rootset/800k_compact_sweep_d8_500/sf_adjudication_top/adjudication_summary.md`

## Root-Set Results

Best active rows:

| config | changes | change rate | node ratio | wall ratio |
|---|---:|---:|---:|---:|
| `800k_qr_s200_eg100` | 106 | 21.20% | 1.0056 | 1.0302 |
| `800k_qr_s200_eg0` | 79 | 15.80% | 1.0067 | 1.0364 |
| `800k_rb_s200_eg100` | 102 | 20.40% | 1.0078 | 1.0399 |
| `800k_rb_s200_eg0` | 76 | 15.20% | 1.0088 | 1.0325 |
| `800k_qr_s100_eg0` | 78 | 15.60% | 1.0150 | 1.0414 |
| `800k_qr_s100_eg100` | 107 | 21.40% | 1.0166 | 1.0402 |
| `400k_qr_s200_eg100_ref` | 107 | 21.40% | 1.0179 | 1.0558 |
| `400k_qr_s400_eg0_ref` | 81 | 16.20% | 1.0215 | 1.0511 |

Inert rows with nominal node savings:

| config family | changes | node ratio | wall ratio |
|---|---:|---:|---:|
| `800k_rb_s800..1500` | 2 / 500 | 0.9986 | 1.0123 to 1.0329 |
| `800k_qr_s400..1500` | 0 / 500 | 1.0000 | 1.0125 to 1.0292 |

The active 800k configs are still wall-negative, but the node penalty is much
smaller than the 400k run. The best active 400k row in this aligned run was
`400k_qr_s200_eg100_ref` at 1.0179 node ratio; the best active 800k row is
`800k_qr_s200_eg100` at 1.0056.

## Stockfish Adjudication

Stockfish depth 12 was run on changed-root positions for the best active rows.

| config | changed roots | SF prefers candidate | avg cp gain |
|---|---:|---:|---:|
| `800k_rb_s200_eg0` | 76 | 57.89% | +10.62 |
| `800k_qr_s100_eg0` | 78 | 58.97% | +9.79 |
| `800k_qr_s100_eg100` | 107 | 56.07% | +10.41 |
| `800k_qr_s200_eg0` | 79 | 55.70% | +9.28 |
| `800k_rb_s200_eg100` | 102 | 54.90% | +7.19 |
| `800k_qr_s200_eg100` | 106 | 53.77% | +6.44 |
| `400k_qr_s200_eg100_ref` | 107 | 50.47% | +2.66 |

The 800k policy is materially better than 400k on changed-root quality. The
main weakness is still runtime: even the best active row is about 3% slower on
wall time in this fixed-depth bench.

## Recommendation

Best pre-match candidate:

`800k_qr_s200_eg100`

Use:

- `PolicyV2File=data/policy/v2/run-stockfish-mpv8-d14d15d10-800k-h256/policy_eg10.bin`
- `PolicyV2Mode=2`
- `PolicyV2Scale=200`
- `PolicyV2QuietScale=200`
- `PolicyV2EndgameScale=100`

Reason: it has the best active node ratio and wall ratio while still changing
21.2% of roots, and Stockfish prefers its changed moves 53.77% of the time.

Not a direct full-SPRT candidate yet. It is good enough for a short mini-match.
If the mini-match is neutral or positive, then promote this config to SPRT.
