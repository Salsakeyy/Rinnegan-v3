#!/usr/bin/env python3
"""Fused offline metrics for policy v2.

The point of this script is to grade a model on the *fused* ranking
`baseline + lambda * model`, not on top-1 prediction in isolation.
v1 reported only top-1; that signal turned out to correlate poorly
with actual search benefit (see docs/policy_v2_audit.md).

Inputs:
  --data       a v2 .npz produced by tools/policy/v2/pack_v2.py
  --model      a torch checkpoint produced by train_v2.py (optional —
               if omitted the script reports baseline-only metrics)
  --lambda     fusion weight: fused = baseline + lambda * model_score
  --top-k      k for NDCG@k (default 5)

Reported metrics, all computed over fused ordering when --model is
given (baseline-only ordering otherwise):

  RR      mean reciprocal rank of the teacher's preferred move
  NDCG@k  graded NDCG using teacher cp scores (or 1/0 for hard targets)
  Spearman rank correlation between fused and teacher orderings (when
           per-move teacher scores are available)
  TopK hit-rate (top1 / top3 / top5)

Segmented breakdowns:
  by bucket    : quiet / capture / check / promotion (uses teacher-move
                 bucket; same convention as v1)
  by phase     : opening / middlegame / endgame from `ctx_features`
  by baseline-rank-of-teacher: bucketed in {1, 2, 3..5, 6..10, 11+}.
                Highlights "where did the engine already get it right
                anyway" vs the cases the model needs to fix.

Custom usefulness metric:
  rank_gain   : (teacher rank under baseline) − (teacher rank under
                fused). Positive means the model promoted the teacher
                move; reported overall and segmented by bucket.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from schema import (  # noqa: E402
    BUCKET_QUIET, BUCKET_CAPTURE, BUCKET_CHECK, BUCKET_PROMO,
    BUCKET_NAMES,
)


# ---------- Data loading ----------

def load_dataset(path: Path) -> dict:
    if path.is_dir():
        out = {}
        for key in (
            "group_offsets", "move_features", "ctx_features",
            "baseline_scores", "move_buckets", "teacher_scores",
            "proposal_mask", "target_indices", "bucket_flags",
        ):
            f = path / f"{key}.npy"
            if f.exists():
                out[key] = np.load(f, allow_pickle=False)
        meta_f = path / "meta.json"
        out["meta"] = json.loads(meta_f.read_text()) if meta_f.exists() else {}
        return out

    data = np.load(path, allow_pickle=False)
    out = {k: data[k] for k in data.files if k != "meta"}
    if "meta" in data.files:
        try:
            out["meta"] = json.loads(str(data["meta"]))
        except Exception:
            out["meta"] = {}
    return out


# ---------- Optional model scoring ----------

def model_scores(d: dict, model_path: Path | None) -> np.ndarray | None:
    if model_path is None:
        return None
    import torch  # local import so the script runs without torch when no model
    from train_v2 import build_model_from_checkpoint  # noqa
    ckpt = torch.load(model_path, map_location="cpu", weights_only=False)
    model = build_model_from_checkpoint(ckpt).eval()

    move_feats = torch.as_tensor(d["move_features"], dtype=torch.float32)
    ctx_feats  = torch.as_tensor(d["ctx_features"],  dtype=torch.float32)
    offsets    = torch.as_tensor(d["group_offsets"],  dtype=torch.long)

    with torch.no_grad():
        # score_groups handles ctx broadcast internally for both architectures
        # and works whether ctx_feats has 0 or >0 columns.
        scores = model.score_groups(move_feats, ctx_feats, offsets)
    return scores.detach().cpu().numpy().astype(np.float32, copy=False)


# ---------- Metric helpers ----------

def rank_of(scores: np.ndarray, target_idx: int) -> int:
    """1-based rank of `target_idx` under descending `scores`."""
    target_score = scores[target_idx]
    # Stable: among ties we count how many are *strictly* greater.
    return int(np.sum(scores > target_score)) + 1


def ndcg_at_k(scores: np.ndarray, gains: np.ndarray, k: int) -> float:
    """Standard NDCG@k over `scores` (descending), with `gains` as the
    relevance vector. Both arrays are aligned per-candidate.
    """
    if len(scores) == 0:
        return float("nan")
    k = min(k, len(scores))
    order = np.argsort(-scores, kind="stable")[:k]
    log_disc = 1.0 / np.log2(np.arange(2, k + 2))
    dcg = float(np.sum(gains[order] * log_disc))
    ideal_order = np.argsort(-gains, kind="stable")[:k]
    idcg = float(np.sum(gains[ideal_order] * log_disc))
    return dcg / idcg if idcg > 0 else float("nan")


def spearman(a: np.ndarray, b: np.ndarray) -> float:
    if len(a) < 2:
        return float("nan")
    ra = np.argsort(np.argsort(-a, kind="stable"))
    rb = np.argsort(np.argsort(-b, kind="stable"))
    ra = ra.astype(np.float64); rb = rb.astype(np.float64)
    ra -= ra.mean(); rb -= rb.mean()
    da = np.linalg.norm(ra); db = np.linalg.norm(rb)
    if da == 0 or db == 0:
        return float("nan")
    return float(np.dot(ra, rb) / (da * db))


# ---------- Per-position evaluation ----------

def evaluate(d: dict, fused: np.ndarray, baseline: np.ndarray, k: int) -> dict:
    offsets = d["group_offsets"]
    targets = d["target_indices"]
    teacher_scores = d.get("teacher_scores")
    move_buckets = d["move_buckets"]
    ctx_feats = d["ctx_features"]
    n = len(offsets) - 1

    rr = np.zeros(n, dtype=np.float64)
    top1 = np.zeros(n, dtype=bool)
    top3 = np.zeros(n, dtype=bool)
    topK = np.zeros(n, dtype=bool)
    ndcg = np.full(n, np.nan, dtype=np.float64)
    spear = np.full(n, np.nan, dtype=np.float64)
    rank_gain = np.zeros(n, dtype=np.float64)
    teacher_bucket = np.zeros(n, dtype=np.uint8)

    # Phase from piece-count / 32. In split mode it's ctx_features[:, 0];
    # in new32_compat the ctx is folded into move_features and piece-count
    # lives at move_features[:, 22] (see pack_policy_dataset.move_features_new32).
    if ctx_feats.ndim == 2 and ctx_feats.shape[1] > 0:
        piece_norm = np.asarray(ctx_feats[:, 0])
    else:
        move_feats = d["move_features"]
        # First move of each group; piece-count is constant within a group.
        piece_norm = np.asarray(move_feats[offsets[:-1], 22])
    phase = np.where(piece_norm > 0.78, 0,
            np.where(piece_norm > 0.42, 1, 2)).astype(np.int64)

    for gi in range(n):
        s, e = int(offsets[gi]), int(offsets[gi + 1])
        target = int(targets[gi])
        f = fused[s:e]
        b = baseline[s:e]
        target_rank = rank_of(f, target)
        baseline_rank = rank_of(b, target)
        rr[gi]    = 1.0 / target_rank
        top1[gi]  = (target_rank == 1)
        top3[gi]  = (target_rank <= 3)
        topK[gi]  = (target_rank <= k)
        rank_gain[gi] = float(baseline_rank - target_rank)
        teacher_bucket[gi] = move_buckets[s + target]

        if teacher_scores is not None:
            ts_slice = np.asarray(teacher_scores[s:e], dtype=np.float64)
            valid = ~np.isnan(ts_slice)
            if valid.sum() >= 2:
                # Use teacher cp directly as a graded gain (clipped to >= 0
                # after centering on the worst-scored move). For hard
                # bestmove labels (only the target has a finite cp) NDCG
                # collapses to top-1 hit on the teacher move, which is
                # fine — that's the natural fallback.
                gains = np.where(valid, ts_slice - np.nanmin(ts_slice), 0.0)
                ndcg[gi] = ndcg_at_k(f, gains, k)
                # Spearman only over scored moves.
                if valid.sum() >= 3:
                    spear[gi] = spearman(f[valid], ts_slice[valid])

    out = {
        "groups": int(n),
        "RR_mean": float(rr.mean()),
        "top1": float(top1.mean()),
        "top3": float(top3.mean()),
        f"top{k}": float(topK.mean()),
        f"NDCG@{k}_mean": float(np.nanmean(ndcg)) if np.any(~np.isnan(ndcg)) else float("nan"),
        "spearman_mean": float(np.nanmean(spear)) if np.any(~np.isnan(spear)) else float("nan"),
        "rank_gain_mean": float(rank_gain.mean()),
    }

    # Segment by bucket of teacher move.
    out["by_bucket"] = {}
    for name, bit in zip(BUCKET_NAMES, (BUCKET_QUIET, BUCKET_CAPTURE, BUCKET_CHECK, BUCKET_PROMO)):
        mask = (teacher_bucket & bit) != 0
        if mask.any():
            out["by_bucket"][name] = {
                "groups": int(mask.sum()),
                "RR_mean": float(rr[mask].mean()),
                "top1":    float(top1[mask].mean()),
                f"top{k}": float(topK[mask].mean()),
                "rank_gain_mean": float(rank_gain[mask].mean()),
            }

    # Segment by phase.
    out["by_phase"] = {}
    for pid, name in enumerate(("opening", "middlegame", "endgame")):
        mask = phase == pid
        if mask.any():
            out["by_phase"][name] = {
                "groups": int(mask.sum()),
                "RR_mean": float(rr[mask].mean()),
                "top1":    float(top1[mask].mean()),
                f"top{k}": float(topK[mask].mean()),
                "rank_gain_mean": float(rank_gain[mask].mean()),
            }

    # Segment by baseline rank of the teacher move (where the model is
    # actually useful: cases where the engine already had it at rank 1
    # don't need the model, the cases at 6+ are the interesting ones).
    out["by_baseline_rank"] = {}
    baseline_target_rank = np.zeros(n, dtype=np.int64)
    for gi in range(n):
        s, e = int(offsets[gi]), int(offsets[gi + 1])
        baseline_target_rank[gi] = rank_of(baseline[s:e], int(targets[gi]))
    bins = [(1, 1, "1"), (2, 2, "2"), (3, 5, "3..5"), (6, 10, "6..10"), (11, 10**9, "11+")]
    for lo, hi, label in bins:
        mask = (baseline_target_rank >= lo) & (baseline_target_rank <= hi)
        if mask.any():
            out["by_baseline_rank"][label] = {
                "groups": int(mask.sum()),
                "RR_mean": float(rr[mask].mean()),
                "top1":    float(top1[mask].mean()),
                f"top{k}": float(topK[mask].mean()),
                "rank_gain_mean": float(rank_gain[mask].mean()),
            }

    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data",   type=Path, required=True)
    parser.add_argument("--model",  type=Path, default=None)
    parser.add_argument("--lambda", dest="lambda_", type=float, default=1.0,
                        help="Fusion weight: fused = baseline + lambda * model_score.")
    parser.add_argument("--top-k",  type=int, default=5)
    parser.add_argument("--output", type=Path, default=None,
                        help="Optional JSON output path. If unset, prints to stdout.")
    args = parser.parse_args()

    d = load_dataset(args.data)
    baseline = np.asarray(d["baseline_scores"], dtype=np.float32)

    ms = model_scores(d, args.model)
    if ms is None:
        fused = baseline.copy()
        title = "baseline-only"
    else:
        fused = baseline + np.float32(args.lambda_) * ms.astype(np.float32, copy=False)
        title = f"fused (lambda={args.lambda_})"

    metrics = evaluate(d, fused, baseline, args.top_k)
    metrics["mode"] = title

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(metrics, indent=2))
    print(json.dumps(metrics, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
