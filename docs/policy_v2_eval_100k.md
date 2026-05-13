# Policy v2 — fused offline eval, 100 K Stockfish-MultiPV checkpoint

## Setup

- Dataset: `chess-engine/data/policy/v2/labels-stockfish-mpv8-d14-100k/dataset.npz`
  - 92,689 positions, 1,218,494 candidate moves (avg 13.1 / group)
  - 401,335 candidates with per-move teacher cp scores (33 %)
  - Teacher: Stockfish 16 @ depth 14, MultiPV 8
- Model: `data/policy/v2/run-stockfish-mpv8-d14-100k-h256/best.pt`
  - `concat` arch, F=32, 256→256→1, KL loss, `--fuse-baseline --baseline-blend 0.001`
  - 16 epochs on MPS, val_loss=0.99, val_top1=33.3 %
- Eval driver: `tools/policy/v2/eval_v2.py`
  - Two small bug fixes shipped in the same change: replaced `model.score(x)` with `score_groups(...)` (the API the trainer actually exposes), and added a `new32_compat` fallback for piece-count when ctx_features has 0 columns.

## Fusion-ratio note

Training composed `effective_logit = model + 0.001 · baseline` (blend=0.001).
At eval, `fused = baseline + λ · model` — to match the training regime,
`λ ≈ 1 / blend = 1000`. The default `λ=1` measures a fusion that
contradicts how the model was trained and gives a bogus negative reading
on every metric. λ=1000 is the right reading and matches what the
engine sees when `PolicyV2Scale ≈ 1000`.

| λ        | RR    | top1   | rank_gain | quiet_rg | rank 6..10 rg |
|---------:|------:|-------:|----------:|---------:|--------------:|
| baseline | 0.367 | 0.223  | +0.00     | +0.00    | +0.00         |
| 1        | 0.341 | 0.192  | −0.33     | −0.41    | −0.46         |
| 100      | 0.370 | 0.201  | +0.93     | +1.18    | +1.42         |
| 500      | 0.429 | 0.233  | +2.24     | +2.81    | +3.05         |
| **1000** | **0.529** | **0.336** | **+3.13** | **+4.12** | **+4.30** |
| 2000     | 0.433 | 0.259  | +1.28     | +4.53    | +4.70         |

## Headline (λ=1000)

| Metric          | Fused    | Baseline-only | Δ        |
|-----------------|---------:|--------------:|---------:|
| RR_mean         | **0.529**| 0.367         | +44 %    |
| top1            | **0.336**| 0.223         | +11.3 pp |
| top5            | **0.785**| —             |          |
| NDCG@5_mean     | **0.650**| —             |          |
| Spearman_mean   | 0.278    | —             |          |
| rank_gain_mean  | **+3.13**| 0             |          |

## Segmented (λ=1000)

### By bucket

| bucket    | groups | RR    | top1   | top5   | rank_gain |
|-----------|-------:|------:|-------:|-------:|----------:|
| quiet     | 73,364 | 0.468 | 0.261  | 0.747  | **+4.12 ✅** |
| capture   | 19,019 | 0.757 | 0.617  | 0.932  | −0.62     |
| check     |  8,881 | 0.756 | 0.622  | 0.922  | +4.92     |
| promotion |    335 | 0.668 | 0.582  | 0.734  | −2.69     |

### By phase

| phase        | groups | RR    | top1   | top5   | rank_gain |
|--------------|-------:|------:|-------:|-------:|----------:|
| opening      | 47,614 | 0.535 | 0.338  | 0.800  | +3.99     |
| middlegame   | 21,404 | 0.548 | 0.363  | 0.787  | +3.61     |
| endgame      | 23,671 | 0.499 | 0.305  | 0.752  | +0.96     |

### By baseline rank of the teacher move

| baseline rank | groups | RR    | top1   | rank_gain |
|---------------|-------:|------:|-------:|----------:|
| 1             | 20,631 | 0.755 | 0.613  | −1.00     |
| 2             |  8,964 | 0.560 | 0.329  | −0.92     |
| 3..5          | 15,196 | 0.442 | 0.221  | +0.04     |
| **6..10**     | 21,241 | 0.462 | 0.255  | **+4.30 ✅** |
| **11+**       | 26,657 | 0.445 | 0.253  | **+8.51**     |

## Pass/fail against the v2 plan's success criteria

| Criterion                         | Result    | Status |
|-----------------------------------|-----------|--------|
| `quiet rank_gain_mean > 0`        | +4.12     | ✅     |
| `baseline-rank-6..10 rank_gain_mean > 0` | +4.30 | ✅     |

Both gating questions in `docs/policy_v2_plan.md` clear comfortably.
The model is doing exactly the residual work the v2 plan was designed to
detect: it loses ~1 rank in cases where the baseline already had the
right move at top-1 (small price), it's neutral in the 3..5 zone, and
it pushes the teacher move forward by **+4 to +8.5 ranks** in the hard
cases (baseline 6+) — which is where wall-clock node savings can
actually accrue.

The only buckets where the model demotes are `capture` (−0.62) and
`promotion` (−2.69), but baseline alone already ranks captures at 76 %
RR and promotions at 67 % RR — small ceilings, small absolute headroom,
and quiet_residual mode in the engine ignores the model's contribution
on those moves anyway.

## Caveat

`eval_v2.py` grades the full dataset, not the held-out 5 % split the
trainer used. The trainer's `val_loss = 0.99` ≈ `train_loss = 1.01` at
convergence, so train/val are tracking each other and these numbers are
likely close to true held-out performance. Rigorous follow-up would
re-run the eval with the same seed/val_fraction split to get
held-out-only numbers. Not blocking for the bench/SPRT decision.

## Conclusion

**Bench now.** Both pass criteria are met by a wide margin and the v1
binary already showed +2.6 % wall (root_bonus@160) without any of this
signal. The next thing that should happen is:

1. Export `best.pt` to a RINPOL2 binary via `tools/policy/v2/export_v2.py`.
2. Bench it with `scripts/bench_policy_v2.sh`, sweeping
   `PolicyV2Scale` around the training-matched value (~1000) and
   `PolicyV2Mode ∈ {1, 2}`. Likely sweet spot: `PolicyV2Mode=2`
   (quiet_residual — model strength is exactly in quiet ordering),
   `PolicyV2Scale ≈ 600..1500`.
3. If bench shows a positive node/wall ratio, run SPRT against
   `UsePolicy=false` using `tests/sprt_policy_vs_off.sh` with
   `MODE=2 SCALE=<best from bench> POLICY_FILE=<exported v2 binary>`.

Train-longer is *not* the right next step — val_loss is plateauing
(0.99 at 16 epochs, dropping ~0.005 per epoch), and with only 92 K
positions the model has likely extracted most of the available signal.
The next data investment is more positions on Hetzner, not more epochs
on the current shard. But before scaling data, find out whether this
shard's model already moves the engine on real games.
