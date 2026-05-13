# Policy v2 — Results: `<run-tag>`

> Standard report layout. Copy this file into `data/policy/v2/<run-tag>/results.md`
> and fill the sections in order. Anything still labeled `<…>` is a TODO.

## 1. Run identity

| Field         | Value                                                      |
|---------------|------------------------------------------------------------|
| Run tag       | `<run-tag>`                                                |
| Date          | `<YYYY-MM-DD>`                                             |
| Engine sha    | `<git-rev-parse HEAD>`                                     |
| Hetzner host  | `<host or N/A>`                                            |
| Runpod host   | `<gpu type / pod id>`                                      |
| Dataset path  | `<data/policy/v2/...>`                                     |
| Checkpoint    | `<.../best.pt>`                                            |
| RINPOL2 binary| `<.../policy.bin>`                                         |

## 2. Teacher / dataset

- Teacher mode: `<stockfish_multipv | rinnegan_deep_multipv | hard_bestmove_fallback>`
- Teacher engine: `<binary + version>`
- Search budget: `depth=<…> nodes=<…> movetime=<…> multipv=<…>`
- Threads / Hash: `<…> / <…> MB`
- Positions labeled: `<N>`
- Proposal mode: `<all_legal | proposal_set>` (`top_quiet=<…>`, `random_quiet=<…>`)
- Feature space: `<split | new32_compat>`

## 3. Training

- Architecture: `<concat h1=… h2=…>` or `<trunk h_trunk=… h_head=…>`
- Loss: `<ce | kl | kl_listwise>` (soft_temp=`<…>`, listwise_weight=`<…>`)
- Fuse baseline: `<on | off>` (blend=`<…>`)
- Batch / epochs / lr: `<…> / <…> / <…>`

Training history (paste from `final.pt`'s `history` list, or summarise):

| epoch | train_loss | val_loss | val_top1 | wall (s) |
|------:|-----------:|---------:|---------:|---------:|
| 1     |            |          |          |          |
| 2     |            |          |          |          |

## 4. Fused offline metrics (`tools/policy/v2/eval_v2.py`)

Run `bash scripts/eval_policy_v2.sh DATA=<dataset> MODEL=<best.pt> LAMBDA=<…>`
and paste the JSON output's headline block here.

```json
<headline RR / top-k / NDCG / spearman / rank_gain>
```

### Segmented

| segment              | groups | RR_mean | top1 | top5 | rank_gain_mean |
|----------------------|-------:|--------:|-----:|-----:|---------------:|
| quiet                |        |         |      |      |                |
| capture              |        |         |      |      |                |
| check                |        |         |      |      |                |
| promotion            |        |         |      |      |                |
| opening              |        |         |      |      |                |
| middlegame           |        |         |      |      |                |
| endgame              |        |         |      |      |                |
| baseline rank=1      |        |         |      |      |                |
| baseline rank 2      |        |         |      |      |                |
| baseline rank 3..5   |        |         |      |      |                |
| baseline rank 6..10  |        |         |      |      |                |
| baseline rank 11+    |        |         |      |      |                |

> Stop-the-line check: if `rank_gain_mean` is ≤ 0 in the `quiet` and
> `baseline rank 6..10` rows, do not deploy. The model is not actually
> moving the teacher move forward where it matters.

## 5. Engine bench (`scripts/bench_policy_v2.sh`)

| config                                | nodes | nps | feature_ns | policy_calls |
|---------------------------------------|------:|----:|-----------:|-------------:|
| off                                   |       |     | 0          | 0            |
| v1_root_bonus_s160                    |       |     |            |              |
| v1_quiet_residual_s160                |       |     |            |              |
| v2_root_bonus_s160_q160               |       |     |            |              |
| v2_quiet_residual_s160_q160           |       |     |            |              |

Derived numbers (vs `off`):

| config                                | node ratio | nps ratio | wall ratio |
|---------------------------------------|-----------:|----------:|-----------:|
| v1_root_bonus_s160                    |            |           |            |
| v1_quiet_residual_s160                |            |           |            |
| v2_root_bonus_s160_q160               |            |           |            |
| v2_quiet_residual_s160_q160           |            |           |            |

> Stop-the-line: `feature_ns / total_search_ns > 2 %` is a hard fail.
> Cut features in pack_v2.py before doing anything else.

## 6. Decision

- [ ] Offline metrics: rank-gain on quiets / baseline-rank-6..10 ≥ 0
- [ ] Engine bench: node ratio < 1.0 AND nps ratio ≥ 0.97 AND wall ratio < 1.0
- [ ] Latency: feature_ns share ≤ 2 %
- [ ] No bench signature mismatch when policy is off

If all four boxes tick, run `tests/sprt_v2_vs_v1.sh` (existing). Otherwise
iterate before SPRT.

## 7. Notes / next experiments

`<one paragraph: what looked promising, what to try next>`
