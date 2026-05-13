#!/usr/bin/env python3
"""Policy v2 trainer.

Differences from v1 (`tools/policy/train_smoke_policy.py`):

  - Reads the v2 dataset format from `pack_v2.py` (separate move / ctx
    feature arrays, per-move teacher_scores, baseline_scores).
  - Two architectures:
      "concat"     a v1-shaped MLP over [move_feats || ctx_feats]. Same
                   shape that the existing engine binary loader expects.
      "trunk"     shared trunk over ctx, move head over move feats,
                   sum at the scalar head. Cheaper per-position at
                   inference because the trunk runs once per group.
                   Engine load support is gated behind RINPOL2 v2+.
  - Three losses:
      "ce"   grouped softmax CE on bestmove (v1-equivalent baseline).
      "kl"   KL(soft_target || softmax(logits)) where soft_target is
             a softmax with temperature `--soft-temp` over teacher cp
             scores (NaN entries get -inf = drop). Falls back to CE
             when no per-move scores exist for a group.
      "kl_listwise" sum of `kl` and an ApproxNDCG-style listwise
             auxiliary that is top-heavy by construction.
  - Optimises against fused-with-baseline ordering when
    `--fuse-baseline` is on: logits passed to the loss become
    `model_logits + baseline_blend * baseline_score`. This biases the
    model toward learning a *residual* on top of the engine's existing
    ordering rather than a from-scratch policy.

The output checkpoint is consumed by `export_v2.py` (writes a RINPOL2
binary that `src/policy.cpp::loadV2` accepts).
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import torch
from torch import nn

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from schema import SCHEMA_VERSION, BUCKET_NAMES  # noqa: E402


# ---------- Data ----------

@dataclass
class V2Dataset:
    move_features:   np.ndarray
    ctx_features:    np.ndarray
    group_offsets:   np.ndarray
    targets:         np.ndarray
    teacher_scores:  np.ndarray
    baseline_scores: np.ndarray
    move_buckets:    np.ndarray
    bucket_flags:    np.ndarray
    meta:            dict

    @property
    def groups(self) -> int: return int(len(self.targets))
    @property
    def f_move(self) -> int: return int(self.move_features.shape[1])
    @property
    def f_ctx(self)  -> int: return int(self.ctx_features.shape[1])


def _coerce(arr: np.ndarray, dtype: np.dtype) -> np.ndarray:
    return arr if arr.dtype == dtype else arr.astype(dtype, copy=False)


def load_dataset(path: Path) -> V2Dataset:
    if path.is_dir():
        def L(name): return np.load(path / f"{name}.npy", allow_pickle=False)
        meta_path = path / "meta.json"
        meta = json.loads(meta_path.read_text()) if meta_path.exists() else {}
        return V2Dataset(
            move_features  =_coerce(L("move_features"),  np.dtype("float32")),
            ctx_features   =_coerce(L("ctx_features"),   np.dtype("float32")),
            group_offsets  =_coerce(L("group_offsets"),  np.dtype("int64")),
            targets        =_coerce(L("target_indices"), np.dtype("int64")),
            teacher_scores =_coerce(L("teacher_scores"), np.dtype("float32")),
            baseline_scores=_coerce(L("baseline_scores"),np.dtype("float32")),
            move_buckets   =_coerce(L("move_buckets"),   np.dtype("uint8")),
            bucket_flags   =_coerce(L("bucket_flags"),   np.dtype("uint8")),
            meta=meta,
        )
    npz = np.load(path, allow_pickle=False)
    meta = {}
    if "meta" in npz.files:
        try:
            meta = json.loads(str(npz["meta"]))
        except Exception:
            meta = {}
    return V2Dataset(
        move_features  =_coerce(npz["move_features"],  np.dtype("float32")),
        ctx_features   =_coerce(npz["ctx_features"],   np.dtype("float32")),
        group_offsets  =_coerce(npz["group_offsets"],  np.dtype("int64")),
        targets        =_coerce(npz["target_indices"], np.dtype("int64")),
        teacher_scores =_coerce(npz["teacher_scores"], np.dtype("float32")),
        baseline_scores=_coerce(npz["baseline_scores"],np.dtype("float32")),
        move_buckets   =_coerce(npz["move_buckets"],   np.dtype("uint8")),
        bucket_flags   =_coerce(npz["bucket_flags"],   np.dtype("uint8")),
        meta=meta,
    )


# ---------- Models ----------

class ConcatPolicy(nn.Module):
    """v1-shaped MLP over [move_feats || ctx_feats]. Engine-compatible."""

    def __init__(self, f_move: int, f_ctx: int, h1: int, h2: int):
        super().__init__()
        self.f_move = f_move
        self.f_ctx  = f_ctx
        self.net = nn.Sequential(
            nn.Linear(f_move + f_ctx, h1), nn.ReLU(),
            nn.Linear(h1, h2),             nn.ReLU(),
            nn.Linear(h2, 1),
        )

    def forward_concat(self, x: torch.Tensor) -> torch.Tensor:
        return self.net(x).squeeze(-1)

    def score_groups(self, move_feats: torch.Tensor, ctx_feats: torch.Tensor,
                     offsets: torch.Tensor) -> torch.Tensor:
        # Broadcast ctx over the moves of each group.
        # offsets: int64[N+1]; lengths: int64[N]
        lengths = offsets[1:] - offsets[:-1]
        ctx_per_move = torch.repeat_interleave(ctx_feats, lengths, dim=0)
        x = torch.cat([move_feats, ctx_per_move], dim=1)
        return self.forward_concat(x)

    def uses_concat(self) -> bool:
        return True


class TrunkPolicy(nn.Module):
    """Shared trunk over ctx + move head; sum at scalar head.

    Per-position cost at inference: 1 trunk forward + L head forwards.
    Significantly cheaper than ConcatPolicy when L is large.
    """

    def __init__(self, f_move: int, f_ctx: int, h_trunk: int, h_head: int):
        super().__init__()
        self.f_move = f_move
        self.f_ctx  = f_ctx
        self.trunk = nn.Sequential(
            nn.Linear(f_ctx, h_trunk), nn.ReLU(),
            nn.Linear(h_trunk, h_trunk), nn.ReLU(),
        )
        self.head = nn.Sequential(
            nn.Linear(f_move + h_trunk, h_head), nn.ReLU(),
            nn.Linear(h_head, 1),
        )

    def score_groups(self, move_feats: torch.Tensor, ctx_feats: torch.Tensor,
                     offsets: torch.Tensor) -> torch.Tensor:
        ctx_h = self.trunk(ctx_feats)
        lengths = offsets[1:] - offsets[:-1]
        ctx_per_move = torch.repeat_interleave(ctx_h, lengths, dim=0)
        x = torch.cat([move_feats, ctx_per_move], dim=1)
        return self.head(x).squeeze(-1)

    def uses_concat(self) -> bool:
        return False


def build_model(args, ds: V2Dataset) -> nn.Module:
    if args.arch == "concat":
        return ConcatPolicy(ds.f_move, ds.f_ctx, args.hidden1, args.hidden2)
    return TrunkPolicy(ds.f_move, ds.f_ctx, args.trunk_hidden, args.head_hidden)


def build_model_from_checkpoint(ckpt: dict) -> nn.Module:
    arch = ckpt["config"]["arch"]
    if arch == "concat":
        m = ConcatPolicy(
            ckpt["config"]["f_move"], ckpt["config"]["f_ctx"],
            ckpt["config"]["hidden1"], ckpt["config"]["hidden2"],
        )
    else:
        m = TrunkPolicy(
            ckpt["config"]["f_move"], ckpt["config"]["f_ctx"],
            ckpt["config"]["trunk_hidden"], ckpt["config"]["head_hidden"],
        )
    m.load_state_dict(ckpt["model"])
    return m


# ---------- Losses ----------

def grouped_softmax_logits(scores: torch.Tensor, offsets: torch.Tensor,
                           max_len: int) -> tuple[torch.Tensor, torch.Tensor]:
    """Scatter pointwise scores into a [N, max_len] dense tensor padded
    with -inf, returning (dense_logits, valid_mask).
    """
    lengths = offsets[1:] - offsets[:-1]
    n = lengths.numel()
    device = scores.device
    rows = torch.repeat_interleave(torch.arange(n, device=device), lengths)
    cumlen = lengths.cumsum(0) - lengths
    cols = torch.arange(scores.numel(), device=device) - cumlen.repeat_interleave(lengths)
    dense = torch.full((n, max_len), float("-inf"), dtype=scores.dtype, device=device)
    dense[rows, cols] = scores
    mask = torch.zeros((n, max_len), dtype=torch.bool, device=device)
    mask[rows, cols] = True
    return dense, mask


def soft_target_distribution(teacher: torch.Tensor, mask: torch.Tensor,
                             temperature: float) -> torch.Tensor | None:
    """Build a softmax-with-temperature soft target from teacher cp
    scores, returning None when no group has any valid teacher score.
    """
    valid_per_group = (~teacher.isnan()) & mask
    has_any = valid_per_group.any(dim=1)
    if not has_any.any():
        return None
    teacher = torch.where(valid_per_group, teacher / max(temperature, 1e-6), torch.full_like(teacher, float("-inf")))
    return torch.softmax(teacher, dim=1), has_any


def loss_fn(args, logits: torch.Tensor, mask: torch.Tensor,
            target_cols: torch.Tensor, teacher_dense: torch.Tensor) -> torch.Tensor:
    masked_logits = logits.masked_fill(~mask, float("-inf"))
    if args.loss == "ce":
        return nn.functional.cross_entropy(masked_logits, target_cols)
    soft_pack = soft_target_distribution(teacher_dense, mask, args.soft_temp)
    if soft_pack is None:
        # Pure CE fallback: no per-move scores in this batch.
        return nn.functional.cross_entropy(masked_logits, target_cols)
    soft, has_any = soft_pack
    log_p = torch.log_softmax(masked_logits, dim=1)
    kl_per_row = torch.where(soft > 0, soft * (soft.clamp(min=1e-12).log() - log_p), torch.zeros_like(soft)).sum(dim=1)
    kl_loss = kl_per_row[has_any].mean() if has_any.any() else torch.tensor(0.0, device=logits.device)

    # Groups without per-move teacher scores still get CE on the
    # bestmove so we don't waste them.
    no_soft = ~has_any
    if no_soft.any():
        ce = nn.functional.cross_entropy(masked_logits[no_soft], target_cols[no_soft])
        loss = (kl_loss * has_any.sum() + ce * no_soft.sum()) / max(1, len(has_any))
    else:
        loss = kl_loss

    if args.loss == "kl_listwise":
        # ApproxNDCG approximation: encourage scores to monotonically
        # match teacher gains. Implemented as a top-heavy pairwise
        # margin: for groups with teacher scores, require
        # logits(top-teacher) > logits(others) - margin.
        if has_any.any():
            t_idx = teacher_dense.argmax(dim=1)
            top_logit = masked_logits.gather(1, t_idx.unsqueeze(1)).squeeze(1)
            margin = 1.0
            others = masked_logits.masked_fill(~mask, float("-inf"))
            # Soft hinge over the top-1 teacher vs all other valid moves.
            diff = (others - top_logit.unsqueeze(1) + margin).clamp(min=0.0)
            diff = diff.masked_fill(~mask, 0.0)
            list_loss = diff.sum(dim=1)
            loss = loss + args.listwise_weight * list_loss[has_any].mean()
    return loss


# ---------- Training loop ----------

def train(args) -> int:
    ds = load_dataset(args.data)
    print(f"loaded v2 dataset: {ds.groups} groups, {ds.move_features.shape[0]} candidates "
          f"(F_move={ds.f_move}, F_ctx={ds.f_ctx}). meta={ds.meta}")

    device = torch.device(args.device if args.device != "auto" else
                          ("cuda" if torch.cuda.is_available() else "cpu"))
    rng = np.random.default_rng(args.seed)

    val_n = max(0, min(int(ds.groups * args.val_fraction), ds.groups - 1))
    perm = rng.permutation(ds.groups)
    val_groups = np.sort(perm[:val_n])
    train_groups = np.sort(perm[val_n:])

    model = build_model(args, ds).to(device)
    history = []
    best_val = math.inf
    start_epoch = 0
    if args.resume_checkpoint is not None:
        ckpt = torch.load(args.resume_checkpoint, map_location="cpu", weights_only=False)
        cfg = ckpt.get("config", {})
        expected = {
            "arch": args.arch,
            "f_move": ds.f_move,
            "f_ctx": ds.f_ctx,
        }
        if args.arch == "concat":
            expected |= {"hidden1": args.hidden1, "hidden2": args.hidden2}
        else:
            expected |= {"trunk_hidden": args.trunk_hidden, "head_hidden": args.head_hidden}
        mismatches = [
            f"{k}: checkpoint={cfg.get(k)!r} current={v!r}"
            for k, v in expected.items()
            if cfg.get(k) != v
        ]
        if mismatches:
            raise ValueError("resume checkpoint is incompatible: " + "; ".join(mismatches))
        model.load_state_dict(ckpt["model"])
        model.to(device)
        history = list(ckpt.get("history") or [])
        if history:
            start_epoch = max(int(item.get("epoch", 0)) for item in history)
            best_val = min(float(item["val"]["loss"]) for item in history if "val" in item and "loss" in item["val"])
        print(f"resumed {args.resume_checkpoint} at epoch {start_epoch}; best_val={best_val:.4f}")
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=args.weight_decay)

    move_t = torch.as_tensor(ds.move_features, dtype=torch.float32, device=device)
    ctx_t  = torch.as_tensor(ds.ctx_features,  dtype=torch.float32, device=device)
    teacher_t  = torch.as_tensor(ds.teacher_scores,  dtype=torch.float32, device=device)
    baseline_t = torch.as_tensor(ds.baseline_scores, dtype=torch.float32, device=device)
    offsets_t  = torch.as_tensor(ds.group_offsets,   dtype=torch.long,    device=device)
    targets_t  = torch.as_tensor(ds.targets,         dtype=torch.long,    device=device)

    def evaluate_split(group_ids: np.ndarray) -> dict:
        if len(group_ids) == 0:
            return {"loss": math.nan, "top1": math.nan}
        model.eval()
        total_loss = 0.0
        total_top1 = 0
        total_groups = 0
        with torch.no_grad():
            for start in range(0, len(group_ids), args.batch_groups):
                gids = group_ids[start:start + args.batch_groups]
                gids_t = torch.as_tensor(gids, dtype=torch.long, device=device)
                slice_starts = offsets_t[gids_t]
                slice_ends   = offsets_t[gids_t + 1]
                lengths      = slice_ends - slice_starts
                row_starts   = lengths.cumsum(0) - lengths
                cand_idx     = torch.repeat_interleave(slice_starts, lengths) + \
                               (torch.arange(int(lengths.sum().item()), device=device) -
                                torch.repeat_interleave(row_starts, lengths))
                local_offsets = torch.cat([torch.zeros(1, dtype=torch.long, device=device), lengths.cumsum(0)])
                m_feats = move_t.index_select(0, cand_idx)
                c_feats = ctx_t.index_select(0, gids_t)
                logits = model.score_groups(m_feats, c_feats, local_offsets)
                if args.fuse_baseline:
                    logits = logits + args.baseline_blend * baseline_t.index_select(0, cand_idx)
                max_len = int(lengths.max().item())
                dense, mask = grouped_softmax_logits(logits, local_offsets, max_len)
                target_cols = targets_t.index_select(0, gids_t)
                teacher_d = torch.full_like(dense, float("nan"))
                # Scatter teacher scores into dense rows.
                rows = torch.repeat_interleave(torch.arange(len(gids), device=device), lengths)
                cumlen = lengths.cumsum(0) - lengths
                cols = torch.arange(len(cand_idx), device=device) - cumlen.repeat_interleave(lengths)
                teacher_d[rows, cols] = teacher_t.index_select(0, cand_idx)
                loss = loss_fn(args, dense, mask, target_cols, teacher_d)
                pred = dense.argmax(dim=1)
                total_top1 += int((pred == target_cols).sum().item())
                total_loss += float(loss.item()) * len(gids)
                total_groups += len(gids)
        return {"loss": total_loss / max(1, total_groups),
                "top1": total_top1 / max(1, total_groups),
                "groups": total_groups}

    args.out_dir.mkdir(parents=True, exist_ok=True)

    for epoch in range(start_epoch + 1, args.epochs + 1):
        model.train()
        rng.shuffle(train_groups)
        ep_start = time.time()
        running_loss = 0.0
        running_groups = 0
        for start in range(0, len(train_groups), args.batch_groups):
            gids = train_groups[start:start + args.batch_groups]
            gids_t = torch.as_tensor(gids, dtype=torch.long, device=device)
            slice_starts = offsets_t[gids_t]
            slice_ends   = offsets_t[gids_t + 1]
            lengths      = slice_ends - slice_starts
            row_starts   = lengths.cumsum(0) - lengths
            cand_idx     = torch.repeat_interleave(slice_starts, lengths) + \
                           (torch.arange(int(lengths.sum().item()), device=device) -
                            torch.repeat_interleave(row_starts, lengths))
            local_offsets = torch.cat([torch.zeros(1, dtype=torch.long, device=device), lengths.cumsum(0)])
            m_feats = move_t.index_select(0, cand_idx)
            c_feats = ctx_t.index_select(0, gids_t)
            logits = model.score_groups(m_feats, c_feats, local_offsets)
            if args.fuse_baseline:
                logits = logits + args.baseline_blend * baseline_t.index_select(0, cand_idx)
            max_len = int(lengths.max().item())
            dense, mask = grouped_softmax_logits(logits, local_offsets, max_len)
            target_cols = targets_t.index_select(0, gids_t)
            teacher_d = torch.full_like(dense, float("nan"))
            rows = torch.repeat_interleave(torch.arange(len(gids), device=device), lengths)
            cumlen = lengths.cumsum(0) - lengths
            cols = torch.arange(len(cand_idx), device=device) - cumlen.repeat_interleave(lengths)
            teacher_d[rows, cols] = teacher_t.index_select(0, cand_idx)
            loss = loss_fn(args, dense, mask, target_cols, teacher_d)

            opt.zero_grad(set_to_none=True)
            loss.backward()
            opt.step()
            running_loss += float(loss.item()) * len(gids)
            running_groups += len(gids)

        train_loss = running_loss / max(1, running_groups)
        val = evaluate_split(val_groups)
        elapsed = time.time() - ep_start
        history.append({"epoch": epoch, "train_loss": train_loss, "val": val, "elapsed": elapsed})
        print(f"epoch {epoch}/{args.epochs}: train_loss={train_loss:.4f} val_loss={val['loss']:.4f} "
              f"val_top1={val['top1']:.4f} ({elapsed:.1f}s)")

        if val["loss"] < best_val:
            best_val = val["loss"]
            ckpt_path = args.out_dir / "best.pt"
            torch.save({
                "model":  model.state_dict(),
                "config": vars(args) | {
                    "f_move": ds.f_move, "f_ctx": ds.f_ctx,
                    "schema_version": SCHEMA_VERSION,
                },
                "history": history,
                "meta": ds.meta,
            }, ckpt_path)

    final = args.out_dir / "final.pt"
    torch.save({
        "model":  model.state_dict(),
        "config": vars(args) | {
            "f_move": ds.f_move, "f_ctx": ds.f_ctx,
            "schema_version": SCHEMA_VERSION,
        },
        "history": history,
        "meta": ds.meta,
    }, final)
    print(f"saved {final}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data",    type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--epochs",  type=int, default=4)
    parser.add_argument("--batch-groups", type=int, default=1024)
    parser.add_argument("--resume-checkpoint", type=Path, default=None,
                        help="Continue training from an existing train_v2 checkpoint; --epochs is total epochs.")
    parser.add_argument("--lr",       type=float, default=3e-4)
    parser.add_argument("--weight-decay", type=float, default=1e-4)
    parser.add_argument("--val-fraction", type=float, default=0.05)
    parser.add_argument("--seed",     type=int, default=0xC0FFEE)
    parser.add_argument("--device",   default="auto")

    parser.add_argument("--arch", choices=["concat", "trunk"], default="concat")
    parser.add_argument("--hidden1", type=int, default=256)
    parser.add_argument("--hidden2", type=int, default=256)
    parser.add_argument("--trunk-hidden", type=int, default=64)
    parser.add_argument("--head-hidden",  type=int, default=128)

    parser.add_argument("--loss", choices=["ce", "kl", "kl_listwise"], default="kl")
    parser.add_argument("--soft-temp", type=float, default=80.0,
                        help="Softmax temperature on teacher cp. Larger = softer targets.")
    parser.add_argument("--listwise-weight", type=float, default=0.1)

    parser.add_argument("--fuse-baseline", action=argparse.BooleanOptionalAction, default=True,
                        help="Train with baseline ordering added into logits so the model "
                             "learns a residual. Strongly recommended for quiet_residual integration.")
    parser.add_argument("--baseline-blend", type=float, default=0.001,
                        help="Multiplier on baseline_scores before summing into logits. "
                             "Baseline scores are O(1e2..1e5); blend keeps them in cross-entropy range.")
    args = parser.parse_args()
    return train(args)


if __name__ == "__main__":
    raise SystemExit(main())
