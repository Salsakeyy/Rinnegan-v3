# Policy v2 — Audit of the v1 pipeline

Snapshot taken on `main` (v5.3). This document only describes what is in the
repo today and what is wrong with it. The forward plan lives in
`policy_v2_plan.md`.

## 1. End-to-end data flow (today)

```
generate_fens.py ─┐
                  ├─► label_uci.py ─► labels.jsonl ─► pack_policy_dataset(.py|_cpp)
extract_lichess.py┘                                       │
                                                          ▼
                                                  features.npz / dir
                                                          │
                                                          ▼
                                                 train_smoke_policy.py
                                                          │
                                                          ▼
                                                 checkpoint-epoch*.pt
                                                          │
                                                          ▼
                                                  export_policy.py
                                                          │
                                                          ▼
                                                   *.bin (RINPOL1)
                                                          │
                                                          ▼
                                          src/policy.cpp  +  src/search.cpp
```

Concrete files:

| Stage         | File                                                  | Notes |
|---------------|-------------------------------------------------------|-------|
| FEN gen       | `tools/policy/generate_fens.py`                       | Random-walk + ply buckets, dedup by 4-field FEN. |
| FEN gen       | `tools/policy/extract_lichess_eval_fens.py`           | Lichess eval-DB filter. |
| Labeling      | `tools/policy/label_uci.py`                           | UCI subprocess; only emits `bestmove`, `pv`, `score`, `mate`, `depth`. **No MultiPV.** |
| Packing       | `tools/policy/pack_policy_dataset.py`                 | Python; one-hot label per group. |
| Packing (fast)| `tools/policy/pack_policy_dataset_cpp.cpp`            | C++ port for speed; same schema. |
| Training      | `tools/policy/train_smoke_policy.py`                  | Pointwise MLP, grouped softmax CE, label smoothing optional. |
| Export        | `tools/policy/export_policy.py`                       | RINPOL1 binary (versions 1 and 2). |
| Engine load   | `src/policy.cpp` (`Policy::load`)                     | Reads RINPOL1; supports legacy / new32 / new64 layouts. |
| Engine apply  | `src/search.cpp::go` + `negamax`                      | One root-only `Policy::scoreMoves` call; bonus added at ply 0. |
| Time mgr      | `src/search.cpp::workerLoop`                          | `policyConfidence` (top1 − top2 margin) shaves soft time. |

## 2. Engine integration — exact behaviour

### Where policy is applied (root only)

`src/search.cpp:748-776` — once per `Search::go` call, before workers spin up:

1. `MoveGen::generateLegal(pos, rootMoves)`
2. `Policy::scoreMoves(pos, ..., shared.rootPolicyBonus)` — fills one `int` bonus per legal root move.
3. `shared.policyConfidence = top1 − top2` over those bonuses.

`src/search.cpp:457-484` — at `ply == 0` inside `negamax`, the cached
`shared.rootPolicyBonus[]` is matched to the current move list (exact-order
fast path or a per-move scan fallback) and passed into `sortMoves`.

### How the bonus is fused into ordering

`src/search.cpp:39-60` — `scoreMove` adds `policyBonus` to **every** move
class identically:

```cpp
if (captured) return 100000 + MVV_LVA[...] + policyBonus;
if (promotion)            return 90000 + ...  + policyBonus;
if (killer[0])            return 80000        + policyBonus;
if (killer[1])            return 79000        + policyBonus;
if (counterMove)          return 78000        + policyBonus;
return history[from][to]                      + policyBonus;
```

This is a **global additive root bonus**. With `PolicyScale=160` (default),
the bonus easily crosses tier thresholds: a "good" quiet can outrank a
losing capture, a "bad" capture can drop below a killer, etc. The bonus
also flat-competes with the `history[]` magnitudes (~bounded by
`HISTORY_MAX`).

### Time-manager hook

`src/search.cpp:702-708` — when `PolicyTimeMod` is on and the policy
distribution has a clear peak, soft time is shaved up to `PolicyTimeBoost`%.

### UCI surface (today)

`src/uci.cpp:316-326, 349-371`:

| Option            | Type     | Default                                                       |
|-------------------|----------|---------------------------------------------------------------|
| `UsePolicy`       | check    | false                                                         |
| `PolicyFile`      | string   | `data/policy/stockfish-200k/train/smoke-policy.bin`           |
| `PolicyScale`     | spin     | 160 (range −10000…10000)                                      |
| `PolicyTimeMod`   | check    | false                                                         |
| `PolicyTimeMargin`| spin     | 150                                                           |
| `PolicyTimeBoost` | spin     | 20 (≤ 80)                                                     |

## 3. Feature & inference cost (production path)

### Feature layouts

`src/policy.cpp:21-25, 233-521`:

- `Legacy` — 17 or 32 floats (basic from/to/piece/MVV-LVA-ish).
- `New32`  — 32 floats with one-hot piece, attacker/defender values, king-distance, `attacks_more_valuable`, `attacks_undefended`, mobility delta, `gives_check`. **Requires one make/unmake per move.**
- `New64`  — 64 floats; superset, also requires make/unmake.

### Per-move work in production

For every legal root move, on every `go`:

1. `pos.makeMove(move, state)` + `pos.unmakeMove(move)` — one full do/undo.
2. `pos.isSquareAttacked(to, them)`, possibly `lowestAttackerValue`, `attackerCount`, `kingZoneAttackCount`, `mobilityCount`, `isPassedPawnAfterMove`, `isRookOpenFile`, `isRookSemiOpenFile` (new64).
3. `staticExchangeScore` — bracket-and-bisect using `SEE::seeGE` (~7 probes worst case) when `isCapture || isPromotion || attackedTo`.
4. `Bitboard attacks = attacksFromPiece(...)`; loop over set bits to compute `attacks_more_valuable` / `attacks_undefended` (new32+).
5. The MLP forward: `input × hidden1 + hidden1 × hidden2 + hidden2 × 1`, naive scalar loop (`linearRelu` at `policy.cpp:523-530`), no SIMD, no quantization.

The MLP itself is tiny (≤ 1024 hidden) but the *feature extraction* is the expensive part — for each of ~30–40 root moves, one make/unmake plus an attacker scan plus several bitboard queries.

### Observed cost vs benefit

`data/policy/new32-10m-f32-h512-runpod/bench_policy_sweep_depth11.md` shows:

| Scale | mean node ratio | mean nps ratio | mean time ratio |
|------:|----------------:|---------------:|----------------:|
| 0     | 1.0000          | 0.9927         | ~1.01           |
| 160   | 0.9985          | 0.9428         | ~1.06           |
| 240   | 0.9159          | 0.9406         | ~0.97           |
| 480   | 0.8828          | 0.9380         | ~0.94           |

NPS drops ~5–8 % from feature extraction alone. Node savings are noisy
and only at large scales begin to compensate for it. Net wall-clock is
neutral / mildly negative — exactly the symptom described in the brief.

## 4. Training pipeline — exact behaviour

### Dataset shape (v1)

Either an `.npz` or directory with standalone `.npy` arrays
(`tools/policy/train_smoke_policy.py:76-109`):

| Array              | Dtype     | Shape                  | Meaning |
|--------------------|-----------|------------------------|---------|
| `features`         | float{16,32} | `[ΣLᵢ, F]`         | Flattened per-(position, move) features. |
| `group_offsets`    | int64     | `[N+1]`                | Position i: `[offsets[i], offsets[i+1])` slice into `features`. |
| `target_indices`   | int64     | `[N]`                  | Index of the teacher bestmove inside the group. |
| `bucket_flags`     | int64     | `[N]`                  | Bit flags on the *teacher move* (1=quiet, 2=cap, 4=check, 8=promo). |
| `labels`           | float32   | `[ΣLᵢ]`                | One-hot bestmove indicator (rarely used by the trainer). |
| `meta` (json)      | str       | scalar                 | `{source, positions, candidates, feature_dim, layout, quiet_only, ...}` |

Notes:

- **Board context is not stored separately** — it is replicated into every
  per-move feature row (e.g. `pf.matBalance`, `pf.pieceCount` get duplicated `Lᵢ` times). Wasteful for storage and for any trunk-style architecture.
- **No baseline ordering score** is stored. Offline evaluation cannot ask
  "would the engine already have ordered this correctly without me?" — the only signal is the teacher index.
- **No per-move teacher score** — the teacher move is a single index per group. Soft-target training is impossible from this format.
- **No proposal mask** — every group is an "all-legal" supervision row.

### Loss

`tools/policy/train_smoke_policy.py:303-348`:

- `grouped_logits` runs the MLP per row, scatters into `[N, max_len]`.
- `weighted_cross_entropy` is plain group-softmax CE on the bestmove index, with optional uniform label-smoothing.
- Bucket-weighting is supported (`--quiet-weight`, etc.) but it weights *positions* (by teacher-move bucket), not candidates.
- No fused-with-baseline metric. No KL-on-soft-target loss. No listwise top-heavy loss (e.g. ApproxNDCG).

### Architecture

`SmokePolicy` (`train_smoke_policy.py:112-124`): `Linear(F, h1) → ReLU → Linear(h1, h2) → ReLU → Linear(h2, 1)`. Pointwise scalar.

The model has no separation between "board context" and "move-local"
inputs and no shared trunk; every row redoes the full forward.

### Export format (RINPOL1)

`export_policy.py` writes:

```
'RINPOL1\0' | u32 version (1 or 2)
[v2: u32 layout_id]
u32 feature_dim | u32 hidden1 | u32 hidden2
f32[h1*F]  w1 | f32[h1] b1
f32[h2*h1] w2 | f32[h2] b2
f32[h2]    w3 | f32     b3
```

Read by `Policy::load` (`src/policy.cpp:544-625`). No bucket calibration,
no phase calibration, no separate trunk, no quantization, no SIMD-friendly
layout.

### Labeling capability today

`tools/policy/label_uci.py` only stores **one** bestmove and a single
`score` per FEN. No MultiPV. The teacher profile selector at
`label_uci.py:127-133` already accepts `stockfish` but does not actually
configure MultiPV or harvest per-line scores. There is a `data/policy/stockfish-200k/` directory of 200k Stockfish-labeled FENs but the labels there are also single-bestmove.

## 5. Diagnosis — why v1 is neutral / negative

This is the diagnosis the user already arrived at. The audit confirms it:

1. **Weak teacher signal.** Stored labels are `(bestmove)` from depth-8 self-search. Even a deeper run gives the same shape — there is no per-move score, no MultiPV, no soft target. Top-1 supervision saturates around 38 % because legal-move sets routinely have several moves of indistinguishable quality.
2. **Wrong offline target.** Training optimises grouped softmax over the bestmove index. It never sees the *baseline* ordering, so it has no incentive to put effort where the engine is already wrong.
3. **Bad integration target.** `policyBonus` is added to every tier identically (MVV-LVA, killers, counters, history). It corrupts cross-bucket ordering instead of refining within a bucket. There is no per-bucket calibration, no phase calibration.
4. **Feature cost dominates the budget.** new32/new64 features force one make/unmake per root move + several attacker scans + repeated SEE bracketing. The bench sweep above shows this costs ~5–8 % NPS, eating the (small) node savings.

## 6. Files / functions to change for v2

Engine side (changes are additive — keep v1 paths for A/B):

| File                | Change                                                                 |
|---------------------|------------------------------------------------------------------------|
| `src/policy.h`      | Add v2 API: `loadV2`, `isV2Loaded`, `scoreRootV2(...)`, perf counters. |
| `src/policy.cpp`    | Add RINPOL2 loader (bucket / phase calibration), keep RINPOL1 path.    |
| `src/uci.h`/`.cpp`  | Add `UsePolicyV2`, `PolicyV2File`, `PolicyV2Mode` (`off`/`root_bonus`/`quiet_residual`), `PolicyV2Scale`, `PolicyV2QuietScale`, `PolicyV2BucketCalib`. |
| `src/search.cpp`    | `scoreMove` accepts `bucketAware` mode flag; in `quiet_residual` mode, `policyBonus` is added only to the `history[from][to]`/killer/counter branches and zeroed for capture/promotion. Wire latency counters around the root scoring call. |
| `src/thread.h`      | Optionally extend `SearchShared` with a per-move bucket cache so workers know which root moves to apply the residual on. |

Tooling side (new files, do not break v1):

| File                                              | Role                              |
|---------------------------------------------------|-----------------------------------|
| `tools/policy/v2/schema.py`                       | v2 dataset schema constants.      |
| `tools/policy/v2/label_multipv.py`                | MultiPV-aware labeler with three modes: `stockfish_multipv`, `rinnegan_deep_multipv`, `hard_bestmove_fallback`. |
| `tools/policy/v2/pack_v2.py`                      | Builds v2 npz with both `all_legal` and `proposal_set` modes; stores baseline ordering, ctx vs move-local features, soft targets. |
| `tools/policy/v2/train_v2.py`                     | Refactored trainer: shared trunk + move head; KL on soft targets; optional ApproxNDCG-style aux. |
| `tools/policy/v2/eval_v2.py`                      | Fused offline metrics (RR, NDCG@k, Spearman, segmented, rank-gain). |
| `tools/policy/v2/export_v2.py`                    | Emits RINPOL2 binary with bucket / phase calibration vectors. |
| `tools/policy/v2/bench_policy.py`                 | Wraps `engine bench` to report nodes / NPS / bestmove deltas at multiple scales. |
| `scripts/hetzner_label_v2.sh`                     | One-shot Hetzner labeling. |
| `scripts/hetzner_pack_v2.sh`                      | One-shot Hetzner packing. |
| `scripts/runpod_train_v2.sh`                      | One-shot Runpod training. |
| `scripts/eval_policy_v2.sh`                       | Fused offline eval + buckets. |
| `scripts/bench_policy_v2.sh`                      | Engine-side bench harness. |
| `docs/policy_v2_results_template.md`              | Standard report layout. |
