# Policy v2 — quiet_residual validation (no retrain)

Engine binary: `build/engine`  
Policy v1 binary: `data/policy/new32-10m-f32-h512-runpod/h512/train/smoke-policy.bin`  
Bench: 16 positions, fixed depth 13, threads=1, hash=16, runs per config: 3  

Node counts are deterministic at fixed depth and single-threaded; NPS / wall are medians over the runs. Bestmove counts are the number of bench positions whose bestmove differs from the `off` baseline.

## Results

| config                | nodes | node ratio | nps | nps ratio | wall (s) | wall ratio | feature_ns | feat % wall | policy_calls | bestmove Δ |
|-----------------------|------:|-----------:|----:|----------:|---------:|-----------:|-----------:|------------:|-------------:|-----------:|
| off                  | 3136877 | 1.0000     |  4273674 | 1.0000    | 1.227    | 1.0000     |          0 |  0.00%      |    0         | 0/16      |
| root_bonus_s160      | 2915260 | 0.9294     |  3366351 | 0.7877    | 1.408    | 1.1474     |  103244540 |  7.33%      |   16         | 3/16      |
| quiet_residual_s160  | 2861543 | 0.9122     |  3697083 | 0.8651    | 1.260    | 1.0267     |   95615374 |  7.59%      |   16         | 4/16      |
| root_bonus_s240      | 3112322 | 0.9922     |  3772511 | 0.8827    | 1.310    | 1.0679     |   95387666 |  7.28%      |   16         | 3/16      |
| quiet_residual_s240  | 3041452 | 0.9696     |  3759520 | 0.8797    | 1.294    | 1.0543     |   95218248 |  7.36%      |   16         | 3/16      |
| root_bonus_s480      | 2984443 | 0.9514     |  3725896 | 0.8718    | 1.287    | 1.0490     |   95363919 |  7.41%      |   16         | 4/16      |
| quiet_residual_s480  | 2962858 | 0.9445     |  3657849 | 0.8559    | 1.318    | 1.0740     |   95403749 |  7.24%      |   16         | 2/16      |

## Headline comparison

- **off**: 3136877 nodes / nps 4273674 / wall 1.227s
- **root_bonus s=160**: 2915260 nodes / nps 3366351 / wall 1.408s → node ratio **0.9294**, nps ratio **0.7877**, wall ratio **1.1474**, bestmove changes **3/16**
- **quiet_residual s=160**: 2861543 nodes / nps 3697083 / wall 1.260s → node ratio **0.9122**, nps ratio **0.8651**, wall ratio **1.0267**, bestmove changes **4/16**

## Conclusion

- **quiet_residual still loses to policy off on wall-clock at scale 160.** The integration change is not enough on its own.
- quiet_residual beats root_bonus on wall-clock — gating the bonus to quiet ordering helps.
- Feature extraction is **7.59%** of search wall in quiet_residual mode (root_bonus: 7.33%). Stop-the-line threshold per `policy_v2_plan.md` is 2%.

### Recommended next step

**(b) cheaper features.** Feature extraction exceeds the 2% wall budget. Cut features in pack_v2.py / src/policy.cpp::fillNew32FeaturesWithCtx that require make/unmake or extra attacker scans before retraining.


---

# Round 2 — after AVX2/NEON-vectorized MLP forward (no feature change, no retrain)

Two changes in `src/policy.cpp` only:

1. Replaced the per-row scalar `linearRelu` with a `layerForward(W, b, in, F, H, out)` kernel that keeps 8 row-accumulators in flight using AVX2 (x86_64) or NEON (arm64) intrinsics, with a scalar fallback. Same arithmetic, ~bit-equivalent (modulo float associativity).
2. Restructured `scoreMoves` / `scoreMovesV2` into two phases — extract all features into a `[count, F]` buffer first, then run the MLP — so the timer can split `feature_ns` from `forward_ns`. The split itself revealed the audit's premise was wrong: feature extraction was negligible all along; the time was all in the matmul.

Same bench, same engine, same v1 binary, runs per config: 5.  

## Results (after optimization)

| config                | nodes | node ratio | nps | nps ratio | wall (s) | wall ratio | feature_ns | forward_ns | bestmove Δ |
|-----------------------|------:|-----------:|----:|----------:|---------:|-----------:|-----------:|-----------:|-----------:|
| off                  | 3136877 | 1.0000     |  4344704 | 1.0000    | 1.201    | 1.0000     |          0 |          0 | 0/16      |
| root_bonus_s160      | 2915260 | 0.9294     |  4146884 | 0.9545    | 1.178    | 0.9809     |     103166 |    8931747 | 3/16      |
| quiet_residual_s160  | 2861543 | 0.9122     |  3903878 | 0.8985    | 1.216    | 1.0124     |     131374 |    9433791 | 4/16      |
| root_bonus_s240      | 3112322 | 0.9922     |  4021087 | 0.9255    | 1.271    | 1.0577     |     136708 |    9267830 | 3/16      |
| quiet_residual_s240  | 3041452 | 0.9696     |  3986175 | 0.9175    | 1.267    | 1.0547     |     140001 |    9426330 | 3/16      |
| root_bonus_s480      | 2984443 | 0.9514     |  3979257 | 0.9159    | 1.256    | 1.0458     |     111958 |    9309041 | 4/16      |
| quiet_residual_s480  | 2962858 | 0.9445     |  3934738 | 0.9056    | 1.252    | 1.0423     |     123791 |    9290624 | 2/16      |

## Before / after (same configurations)

| config                | nps before | nps after | nps Δ | wall before | wall after | wall Δ |
|-----------------------|-----------:|----------:|------:|------------:|-----------:|-------:|
| off                  |  4273674 |  4344704 |  +1.7% | 1.227      | 1.201     |  -2.1% |
| root_bonus_s160      |  3366351 |  4146884 | +23.2% | 1.408      | 1.178     | -16.3% |
| quiet_residual_s160  |  3697083 |  3903878 |  +5.6% | 1.260      | 1.216     |  -3.5% |
| root_bonus_s240      |  3772511 |  4021087 |  +6.6% | 1.310      | 1.271     |  -3.0% |
| quiet_residual_s240  |  3759520 |  3986175 |  +6.0% | 1.294      | 1.267     |  -2.1% |
| root_bonus_s480      |  3725896 |  3979257 |  +6.8% | 1.287      | 1.256     |  -2.4% |
| quiet_residual_s480  |  3657849 |  3934738 |  +7.6% | 1.318      | 1.252     |  -5.0% |

## Headline

Engine-time wall (nodes / nps) factors out subprocess startup overhead:

- **off**:                  0.7220 s of search, nps 4.34 M
- **root_bonus s=160**:     0.7030 s of search, nps 4.15 M → **engine-time wall 0.9737× of off** ✅
- **quiet_residual s=160**: 0.7330 s of search, nps 3.90 M → engine-time wall 1.0152×

**`root_bonus s=160` now beats policy off by 2.6% on engine wall-clock.** This is the same v1 binary; the only thing that changed is how we run its forward pass. At scale 240 and 480 both modes still lose vs off (search disturbance grows faster than node savings), so 160 is the sweet spot.

Latency split for quiet_residual s=160: feature **131 µs** (0.011% of wall) + forward **9.43 ms** (0.78% of wall). The audit's premise was inverted — feature extraction was never the cost. The matmul was, and an AVX2/NEON kernel + a two-phase split made it ~10× cheaper with no model change.

The flip between the two modes is also informative. Pre-optimization, the MLP cost dominated and `quiet_residual` looked better than `root_bonus` because it skipped the bonus on captures (which were most disturbed by the per-move overhead). Post-optimization, the MLP cost is negligible, so the two modes are now graded purely on *search quality*, and the v1 binary turns out to be more useful at the cross-bucket level than at the within-bucket-quiet level on this bench.

## Conclusion

- **The v1 policy is wall-clock positive after this engine change**, with no retrain. `root_bonus s=160` saves ~7% of nodes at only ~4.5% NPS cost, net +2.6% wall.
- The earlier "cheaper features first" recommendation in this same file was based on a false premise. The bench sweep at `data/policy/new32-10m-f32-h512-runpod/bench_policy_sweep_depth11.md` (and Round 1 above) attributed all of the policy cost to feature extraction because the timer was bundling both phases. Splitting the timer made it obvious the matmul was the cost.
- `quiet_residual` is now the worse of the two modes on this bench. It is still the safer default in pathological positions because it cannot reorder captures, but it is no longer the obvious A/B winner.

### Recommended next step

1. **Re-pin `BENCH_SIGNATURE`** if you decide to ship `UsePolicy=true PolicyMode=1 PolicyScale=160` as the production default. The off-policy signature (`3,136,877`) is unchanged; the policy-on path is a different node count by construction.
2. **Run SPRT** (`tests/sprt_v2_vs_v1.sh`) of `UsePolicy=true PolicyMode=1 PolicyScale=160` against `UsePolicy=false`. Bench predicts +2.6% wall; SPRT will say whether that survives gauntlet noise.
3. **Only after SPRT** return to the v2 plan: stronger Stockfish-MultiPV labels, KL training, fused metrics. The bottleneck moved from "engine integration / inference cost" to "model quality" — exactly where the v2 plan said the work should go *after* integration was solved.
