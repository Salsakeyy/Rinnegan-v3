# Policy v2 — Plan

Goal: build the smallest end-to-end pipeline that can answer the question
"is a learned move-ordering residual saving real wall-clock?" and is
honest about latency. v1 is preserved untouched. v2 lives in
`tools/policy/v2/` and behind new UCI options.

## Smallest first change

The **smallest high-value change** is the engine-side gating: switch
v1 from "global additive root bonus" to **`quiet_residual`** in
`src/search.cpp::scoreMove`. This is one pure-engine patch, costs zero
training, and tests the integration hypothesis:

> If the bonus is what's eating NPS-for-nothing, applying it only to
> quiet ordering should already shrink the regression (no model retrain,
> no new dataset). Even with the existing v1 binary loaded.

Concretely: add a `PolicyMode` UCI knob (`off|root_bonus|quiet_residual`,
default `root_bonus`). In `quiet_residual` mode, `policyBonus` is added
only when the move is non-capture and non-promotion. Bench at scales
80/160/240/320 vs the v1 sweep table to see whether quiet-only gating
already moves node and NPS ratios in the right direction.

Once that A/B is run, build the rest of the v2 pipeline in the order
below.

## Implementation order (small, reviewable steps)

1. **Audit + plan.** This document and `policy_v2_audit.md`.
2. **Engine: `quiet_residual` gating + new UCI options.** No model
   change yet — just a different way to consume the same `RINPOL1`
   binary. Adds latency timers around root policy scoring.
3. **v2 dataset format.** `tools/policy/v2/pack_v2.py` and the
   schema in `schema.py`. Both `all_legal` and `proposal_set` modes.
   v1 is left untouched.
4. **v2 fused offline metrics.** `tools/policy/v2/eval_v2.py` —
   the policy is graded by `RR / NDCG@k / Spearman / segmented / rank-gain`
   on the **fused** ordering (`baseline + λ · model`), not on raw top-1.
5. **Stronger-teacher labeler.** `tools/policy/v2/label_multipv.py`
   with three modes: `stockfish_multipv`, `rinnegan_deep_multipv`,
   `hard_bestmove_fallback`. Stockfish multipv mode harvests one cp
   score per principal variation; the packer turns those into
   softmax-with-temperature soft targets.
6. **Trainer refactor.** `tools/policy/v2/train_v2.py`:
   - Shared trunk over board context, move head over move-local features.
   - Pointwise baseline loss kept as a config option for sanity.
   - Soft-target KL loss as the new default.
   - Optional listwise top-heavy auxiliary (ApproxNDCG-style).
7. **RINPOL2 export + engine loader.** v2 binary embeds bucket calibration
   (`f32[buckets]` scale + bias) and phase calibration (`f32[phases]`).
8. **Engine: model side of `quiet_residual`.** Switch the root scoring
   call to use the v2 binary when loaded; bucket-aware scaling.
9. **Bench harness.** `scripts/bench_policy_v2.sh` runs `engine bench`
   at scale ∈ {off, 0, 80, 160, 240, 320, 480, 640, 960, 1280} and
   produces the same Markdown table shape as today's sweep.
10. **Calibration pass.** Use `eval_v2.py` segmented-by-phase metrics to
    populate `phase_scale[]` in the binary, then re-bench. Only after
    this looks good move on to architecture experiments.

I do **not** plan a transformer or any "big architecture" step until
steps 1–10 land and the bench shows positive node + NPS deltas. The
brief explicitly disallows it.

## Machine split

### Hetzner (CPU)

- `scripts/hetzner_label_v2.sh` — generate FENs, run Stockfish or
  Rinnegan as teacher (multipv 5–8, depth 16+ for SF, deep nodes for
  Rinnegan). Resumable, append-only JSONL.
- `scripts/hetzner_pack_v2.sh` — Python packer or, when the C++ packer
  is updated, `pack_policy_dataset_cpp` in v2 mode.
- `scripts/bench_policy_v2.sh` — runs the engine bench sweep + node /
  NPS / bestmove deltas vs `off`.
- `tests/sprt_v2_vs_v1.sh` (already exists) — final SPRT once a v2
  binary looks promising. **No SPRT until the bench numbers say so.**

### Runpod (GPU)

- `scripts/runpod_train_v2.sh` — `train_v2.py`. Soft-target KL or
  pointwise baseline. Saves checkpoint + config + meta JSON.
- Ablations (KL temperature, bucket weighting, trunk vs no-trunk,
  proposal-set vs all-legal) all run as Runpod sweeps, each producing a
  tagged checkpoint plus a `summary.json`.
- Final step: `tools/policy/v2/export_v2.py` to write the RINPOL2 binary
  (small, fits in the repo or in `data/policy/v2/<tag>/`).

Hetzner never trains. Runpod never labels.

## File / function targets

Engine:

- `src/policy.h`, `src/policy.cpp` — add v2 loader, perf counters,
  bucket-aware scoring.
- `src/uci.h`, `src/uci.cpp` — new UCI options.
- `src/search.cpp` — quiet_residual mode in `scoreMove`, latency timers
  around `Policy::scoreMoves` at the root.

Tooling (all under `tools/policy/v2/`):

- `schema.py` — schema_version, field names, layout helpers.
- `label_multipv.py` — UCI MultiPV labeler with three modes.
- `pack_v2.py` — packs JSONL → `.npz` v2.
- `train_v2.py` — soft-target trainer (KL + optional listwise aux).
- `eval_v2.py` — fused offline metrics.
- `export_v2.py` — RINPOL2 export.
- `bench_policy.py` — bench harness.

Scripts:

- `scripts/hetzner_label_v2.sh`, `scripts/hetzner_pack_v2.sh`,
  `scripts/runpod_train_v2.sh`, `scripts/eval_policy_v2.sh`,
  `scripts/bench_policy_v2.sh`.

## Public dataset stance

The brief explicitly cautions against assuming a public Stockfish-labeled
dataset is "good enough by default." The audit documented that today
only single-bestmove labels exist in `data/policy/stockfish-200k/`, even
though Stockfish was the teacher. **We do not import a public dataset
in v2 unless it carries**:

- A proposal set covering at least ~10 candidate moves per position
  (or all legal moves), and
- A per-move score (cp / win-rate), not just bestmove, and
- Reproducible teacher metadata (engine, depth/nodes/movetime, multipv,
  hash, threads, SF NNUE hash).

Anything else is bootstrap material at most.

## Latency discipline

`src/policy.cpp` will gain two atomic counters wrapped around the root
scoring call:

- `featureNanos` — total ns spent in `fillNew*FeaturesWithCtx`.
- `forwardNanos` — total ns spent in the MLP.

`scripts/bench_policy_v2.sh` will print these alongside `nodes` /
`nps`. If `featureNanos` exceeds 2 % of the search budget at fixed
depth, that is a stop-the-line signal: cut features (drop new64 entries
that require make/unmake, prefer cached SEE shortcuts) before doing
anything else.

## Success criteria (mirrors the brief)

- v2 dataset packer produces `.npz` shards with the v2 schema and
  round-trips through `train_v2.py` → `export_v2.py` → engine load.
- A multipv labeling job runs end-to-end on Hetzner.
- A training job runs end-to-end on Runpod and produces a RINPOL2 binary.
- `eval_v2.py` reports fused metrics segmented by bucket and phase.
- `bench_policy_v2.sh` produces a Markdown table comparable to v1's
  `bench_policy_sweep_depth*.md`, with `quiet_residual` and `root_bonus`
  rows side by side.
- v1 path (`UsePolicy`, `PolicyFile`, `PolicyScale`, `PolicyTimeMod`)
  still works unmodified; CCRL build is unaffected.
