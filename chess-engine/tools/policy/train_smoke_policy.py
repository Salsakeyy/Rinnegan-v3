#!/usr/bin/env python3
"""Train a grouped legal-move policy scorer."""

from __future__ import annotations

import argparse
import contextlib
import json
import math
import os
import sys
import time
import warnings
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import torch
from torch import nn


MAX_ENGINE_HIDDEN = 1024


def resolve_device(name: str) -> torch.device:
    if name != "auto":
        return torch.device(name)
    if torch.cuda.is_available():
        return torch.device("cuda")
    if hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
        return torch.device("mps")
    return torch.device("cpu")


def read_meta_value(raw) -> dict:
    if raw is None:
        return {}
    if isinstance(raw, np.ndarray):
        raw = raw.item()
    try:
        return json.loads(str(raw))
    except json.JSONDecodeError:
        return {}


@dataclass
class PolicyDataset:
    features: np.ndarray
    offsets: np.ndarray
    targets: np.ndarray
    bucket_flags: np.ndarray | None
    meta: dict
    path: Path
    mmap: bool
    keepalive: object | None = None

    @property
    def groups(self) -> int:
        return int(len(self.targets))

    @property
    def candidates(self) -> int:
        return int(len(self.features))

    @property
    def feature_dim(self) -> int:
        return int(self.features.shape[1])


def array_with_dtype(array: np.ndarray, dtype: np.dtype) -> np.ndarray:
    if array.dtype == dtype:
        return array
    return array.astype(dtype, copy=False)


def load_dataset(path: Path, mmap: bool) -> PolicyDataset:
    if path.is_dir():
        mmap_mode = "r" if mmap else None
        features = np.load(path / "features.npy", mmap_mode=mmap_mode, allow_pickle=False)
        if not np.issubdtype(features.dtype, np.floating):
            raise RuntimeError(f"features.npy must be floating point, got {features.dtype}")
        offsets = np.load(path / "group_offsets.npy", mmap_mode=mmap_mode, allow_pickle=False)
        targets = np.load(path / "target_indices.npy", mmap_mode=mmap_mode, allow_pickle=False)
        bucket_path = path / "bucket_flags.npy"
        bucket_flags = np.load(bucket_path, mmap_mode=mmap_mode, allow_pickle=False) if bucket_path.exists() else None
        meta_path = path / "meta.json"
        meta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.exists() else {}
        return PolicyDataset(
            features=features,
            offsets=array_with_dtype(offsets, np.dtype("int64")),
            targets=array_with_dtype(targets, np.dtype("int64")),
            bucket_flags=array_with_dtype(bucket_flags, np.dtype("int64")) if bucket_flags is not None else None,
            meta=meta,
            path=path,
            mmap=mmap,
        )

    data = np.load(path, allow_pickle=False)
    features = data["features"]
    if not np.issubdtype(features.dtype, np.floating):
        raise RuntimeError(f"features must be floating point, got {features.dtype}")
    offsets = array_with_dtype(data["group_offsets"], np.dtype("int64"))
    targets = array_with_dtype(data["target_indices"], np.dtype("int64"))
    bucket_flags = array_with_dtype(data["bucket_flags"], np.dtype("int64")) if "bucket_flags" in data.files else None
    meta = read_meta_value(data["meta"]) if "meta" in data.files else {}
    meta_sidecar = path.with_suffix(path.suffix + ".meta.json")
    if not meta and meta_sidecar.exists():
        meta = json.loads(meta_sidecar.read_text(encoding="utf-8"))
    return PolicyDataset(features, offsets, targets, bucket_flags, meta, path, False, data)


class SmokePolicy(nn.Module):
    def __init__(self, feature_dim: int, hidden1: int, hidden2: int):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(feature_dim, hidden1),
            nn.ReLU(),
            nn.Linear(hidden1, hidden2),
            nn.ReLU(),
            nn.Linear(hidden2, 1),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.net(x).squeeze(-1)


def parse_hidden(value: str, hidden2: int | None) -> tuple[int, int]:
    normalized = value.lower().replace(",", "x").replace(":", "x")
    if "x" in normalized:
        parts = [part for part in normalized.split("x") if part]
        if len(parts) != 2:
            raise ValueError(f"invalid --hidden architecture: {value}")
        h1, h2 = int(parts[0]), int(parts[1])
        if hidden2 is not None and hidden2 != h2:
            raise ValueError("--hidden2 conflicts with two-layer --hidden value")
    else:
        h1 = int(normalized)
        h2 = h1 if hidden2 is None else int(hidden2)
    if h1 <= 0 or h2 <= 0:
        raise ValueError("hidden sizes must be positive")
    if h1 > MAX_ENGINE_HIDDEN or h2 > MAX_ENGINE_HIDDEN:
        raise ValueError(f"engine policy runtime supports hidden sizes up to {MAX_ENGINE_HIDDEN}")
    return h1, h2


def resolve_val_count(n: int, val_fraction: float, val_groups: int, max_val_groups: int) -> int:
    if n <= 1:
        return 0
    if val_groups > 0:
        count = val_groups
    else:
        count = int(n * val_fraction)
        if n >= 5 and count == 0 and val_fraction > 0:
            count = 1
    if max_val_groups > 0:
        count = min(count, max_val_groups)
    return min(max(0, count), n - 1)


def split_groups(n: int, args) -> tuple[np.ndarray, np.ndarray]:
    rng = np.random.default_rng(args.seed)
    groups = np.arange(n, dtype=np.int64)
    rng.shuffle(groups)
    val_n = resolve_val_count(n, args.val_fraction, args.val_groups, args.max_val_groups)
    val_groups = np.sort(groups[:val_n])
    train_groups = np.sort(groups[val_n:])
    if len(train_groups) == 0:
        raise RuntimeError("not enough groups to train")
    return train_groups, val_groups


def sample_groups(groups: np.ndarray, max_groups: int, seed: int) -> np.ndarray:
    if max_groups <= 0 or len(groups) <= max_groups:
        return groups
    rng = np.random.default_rng(seed)
    selected = rng.choice(groups, size=max_groups, replace=False)
    return np.sort(selected)


def iter_group_batches(
    groups: np.ndarray,
    batch_size: int,
    shuffle: str,
    rng: np.random.Generator,
    max_groups: int = 0,
):
    if len(groups) == 0:
        return

    if shuffle == "groups":
        if max_groups > 0 and max_groups < len(groups):
            values = rng.choice(groups, size=max_groups, replace=False)
        else:
            values = groups.copy()
            rng.shuffle(values)
        for start in range(0, len(values), batch_size):
            yield values[start : start + batch_size]
        return

    block_count = (len(groups) + batch_size - 1) // batch_size
    order = np.arange(block_count, dtype=np.int64)
    if shuffle == "blocks":
        rng.shuffle(order)

    emitted = 0
    for block in order:
        start = int(block) * batch_size
        end = min(start + batch_size, len(groups))
        if max_groups > 0:
            remaining = max_groups - emitted
            if remaining <= 0:
                break
            end = min(end, start + remaining)
        batch = groups[start:end]
        emitted += len(batch)
        yield batch


@dataclass
class PreparedBatch:
    group_ids: np.ndarray
    features: np.ndarray
    rows: np.ndarray
    cols: np.ndarray
    target_cols: np.ndarray
    weights: np.ndarray | None
    max_len: int
    local_candidate_idx: np.ndarray | None
    candidates: int
    overfetched: int


def prepare_batch(
    dataset: PolicyDataset,
    group_ids: np.ndarray,
    overfetch_ratio: float,
    group_weights: np.ndarray | None = None,
) -> PreparedBatch:
    offsets = dataset.offsets
    starts = offsets[group_ids]
    ends = offsets[group_ids + 1]
    lengths = ends - starts
    max_len = int(lengths.max())
    total_candidates = int(lengths.sum())

    rows = np.repeat(np.arange(len(group_ids), dtype=np.int64), lengths)
    row_candidate_starts = np.repeat(np.cumsum(lengths, dtype=np.int64) - lengths, lengths)
    cols = np.arange(total_candidates, dtype=np.int64) - row_candidate_starts

    local_candidate_idx: np.ndarray | None = None
    span_start = int(starts[0])
    span_end = int(ends[-1])
    span_candidates = span_end - span_start
    monotonic_groups = len(group_ids) == 1 or bool(np.all(group_ids[1:] >= group_ids[:-1]))
    if monotonic_groups and span_candidates <= max(1, int(math.ceil(total_candidates * overfetch_ratio))):
        features = dataset.features[span_start:span_end]
        if span_candidates != total_candidates:
            local_candidate_idx = starts[rows] + cols - span_start
    else:
        candidate_idx = starts[rows] + cols
        features = dataset.features[candidate_idx]

    return PreparedBatch(
        group_ids=np.asarray(group_ids),
        features=np.asarray(features),
        rows=rows,
        cols=cols,
        target_cols=np.asarray(dataset.targets[group_ids]),
        weights=None if group_weights is None else np.asarray(group_weights[group_ids], dtype=np.float32),
        max_len=max_len,
        local_candidate_idx=local_candidate_idx,
        candidates=total_candidates,
        overfetched=int(len(features) - total_candidates),
    )


def batch_to_device(batch: PreparedBatch, device: torch.device):
    feature_np = np.asarray(batch.features)
    if feature_np.dtype != np.float32:
        feature_np = feature_np.astype(np.float32, copy=False)
    with warnings.catch_warnings():
        warnings.filterwarnings("ignore", message="The given NumPy array is not writable")
        features = torch.as_tensor(feature_np, dtype=torch.float32).to(device, non_blocking=True)
    rows = torch.as_tensor(batch.rows, dtype=torch.long, device=device)
    cols = torch.as_tensor(batch.cols, dtype=torch.long, device=device)
    target_cols = torch.as_tensor(batch.target_cols, dtype=torch.long, device=device)
    weights = None if batch.weights is None else torch.as_tensor(batch.weights, dtype=torch.float32, device=device)
    if batch.local_candidate_idx is None:
        local_idx = None
    else:
        local_idx = torch.as_tensor(batch.local_candidate_idx, dtype=torch.long, device=device)
    return features, rows, cols, target_cols, weights, local_idx


def amp_context(device: torch.device, enabled: bool):
    if not enabled:
        return contextlib.nullcontext()
    if device.type == "cuda":
        return torch.autocast(device_type="cuda", dtype=torch.float16)
    return contextlib.nullcontext()


def grouped_logits(
    model: nn.Module,
    batch: PreparedBatch,
    device: torch.device,
    use_amp: bool,
) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor | None]:
    features, rows, cols, target_cols, weights, local_idx = batch_to_device(batch, device)
    with amp_context(device, use_amp):
        logits = model(features)
        if local_idx is not None:
            logits = logits[local_idx]
    if logits.dtype != torch.float32:
        logits = logits.float()
    with amp_context(device, False):
        grouped = logits.new_full((len(batch.target_cols), batch.max_len), -1.0e9)
        grouped[rows, cols] = logits
    return grouped, target_cols, weights


def weighted_cross_entropy(
    grouped: torch.Tensor,
    target_cols: torch.Tensor,
    weights: torch.Tensor | None,
    label_smoothing: float,
) -> torch.Tensor:
    if label_smoothing > 0.0:
        log_probs = torch.log_softmax(grouped, dim=1)
        row_ids = torch.arange(target_cols.numel(), device=target_cols.device)
        nll = -log_probs[row_ids, target_cols]
        valid = grouped > -1.0e8
        valid_counts = torch.clamp(valid.sum(dim=1), min=1)
        smooth = -(log_probs.masked_fill(~valid, 0.0).sum(dim=1) / valid_counts)
        losses = (1.0 - label_smoothing) * nll + label_smoothing * smooth
        if weights is None:
            return losses.mean()
        denom = torch.clamp(weights.sum(), min=1.0)
        return (losses * weights).sum() / denom
    if weights is None:
        return nn.functional.cross_entropy(grouped, target_cols)
    losses = nn.functional.cross_entropy(
        grouped,
        target_cols,
        reduction="none",
    )
    denom = torch.clamp(weights.sum(), min=1.0)
    return (losses * weights).sum() / denom


def evaluate(
    model: nn.Module,
    dataset: PolicyDataset,
    group_ids: np.ndarray,
    batch_groups: int,
    device: torch.device,
    args,
    use_amp: bool,
) -> dict:
    model.eval()
    total_loss = 0.0
    top1 = 0
    top3 = 0
    total = 0
    candidates = 0
    overfetched = 0
    rng = np.random.default_rng(args.seed)
    with torch.no_grad():
        for batch_groups_ids in iter_group_batches(group_ids, batch_groups, "none", rng):
            batch = prepare_batch(dataset, batch_groups_ids, args.contiguous_read_overfetch)
            grouped, target_cols, _ = grouped_logits(model, batch, device, use_amp)
            loss = nn.functional.cross_entropy(grouped, target_cols)
            topk = torch.topk(grouped, k=min(3, batch.max_len), dim=1).indices
            total_loss += float(loss.item()) * len(batch_groups_ids)
            top1 += int((topk[:, 0] == target_cols).sum().item())
            top3 += int((topk == target_cols.unsqueeze(1)).any(dim=1).sum().item())
            total += len(batch_groups_ids)
            candidates += batch.candidates
            overfetched += batch.overfetched

    if total == 0:
        return {"loss": math.nan, "top1": math.nan, "top3": math.nan, "avg_candidates": math.nan, "groups": 0}
    return {
        "loss": total_loss / total,
        "top1": top1 / total,
        "top3": top3 / total,
        "avg_candidates": candidates / total,
        "groups": int(total),
        "overfetched_candidates": int(overfetched),
    }


def evaluate_buckets(
    model: nn.Module,
    dataset: PolicyDataset,
    group_ids: np.ndarray,
    batch_groups: int,
    device: torch.device,
    args,
    use_amp: bool,
):
    if dataset.bucket_flags is None or len(group_ids) == 0:
        return {}
    buckets = {
        "quiet": 1,
        "capture": 2,
        "check": 4,
        "promotion": 8,
    }
    out = {}
    for name, bit in buckets.items():
        selected = group_ids[(dataset.bucket_flags[group_ids] & bit) != 0]
        selected = sample_groups(selected, args.bucket_eval_groups, args.seed + bit)
        metrics = evaluate(model, dataset, selected, batch_groups, device, args, use_amp)
        out[name] = metrics
    return out


def collect_misses(
    model: nn.Module,
    dataset: PolicyDataset,
    group_ids: np.ndarray,
    batch_groups: int,
    device: torch.device,
    args,
    use_amp: bool,
) -> np.ndarray:
    if len(group_ids) == 0:
        return np.asarray([], dtype=np.int64)
    model.eval()
    misses: list[np.ndarray] = []
    rng = np.random.default_rng(args.seed)
    with torch.no_grad():
        for batch_group_ids in iter_group_batches(group_ids, batch_groups, "none", rng):
            batch = prepare_batch(dataset, batch_group_ids, args.contiguous_read_overfetch)
            grouped, target_cols, _ = grouped_logits(model, batch, device, use_amp)
            pred = torch.argmax(grouped, dim=1)
            mask = (pred != target_cols).detach().cpu().numpy()
            if np.any(mask):
                misses.append(np.asarray(batch_group_ids)[mask])
    if not misses:
        return np.asarray([], dtype=np.int64)
    return np.concatenate(misses).astype(np.int64, copy=False)


def load_hard_examples(path: str, groups: int) -> np.ndarray:
    if not path:
        return np.asarray([], dtype=np.int64)
    raw = np.load(path, allow_pickle=False)
    values = np.asarray(raw, dtype=np.int64).reshape(-1)
    return values[(values >= 0) & (values < groups)]


def build_group_weights(dataset: PolicyDataset, args) -> np.ndarray | None:
    weights = np.ones(dataset.groups, dtype=np.float32)
    changed = False

    if dataset.bucket_flags is not None:
        bucket_specs = [
            (1, args.quiet_weight),
            (2, args.capture_weight),
            (4, args.check_weight),
            (8, args.promotion_weight),
        ]
        for bit, value in bucket_specs:
            if value != 1.0:
                weights[(dataset.bucket_flags & bit) != 0] *= np.float32(value)
                changed = True

    hard_examples = load_hard_examples(args.hard_examples, dataset.groups)
    if len(hard_examples) > 0 and args.hard_example_weight != 1.0:
        weights[hard_examples] *= np.float32(args.hard_example_weight)
        changed = True

    return weights if changed else None


def latest_checkpoint(out_dir: Path) -> Path | None:
    checkpoints = sorted(out_dir.glob("checkpoint-epoch*.pt"))
    return checkpoints[-1] if checkpoints else None


def save_training_checkpoint(
    path: Path,
    raw_model: nn.Module,
    optimizer: torch.optim.Optimizer,
    scaler,
    epoch: int,
    history: list[dict],
    dataset: PolicyDataset,
    hidden1: int,
    hidden2: int,
    config: dict,
) -> None:
    payload = {
        "model": raw_model.state_dict(),
        "optimizer": optimizer.state_dict(),
        "scaler": None if scaler is None else scaler.state_dict(),
        "epoch": epoch,
        "history": history,
        "feature_dim": dataset.feature_dim,
        "hidden": hidden1 if hidden1 == hidden2 else None,
        "hidden1": hidden1,
        "hidden2": hidden2,
        "meta": dataset.meta,
        "config": config,
    }
    torch.save(payload, path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, required=True,
                        help="Policy dataset .npz or a directory containing standalone .npy arrays.")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--epochs", type=int, default=4)
    parser.add_argument("--batch-groups", type=int, default=4096)
    parser.add_argument("--micro-batch-groups", type=int, default=0,
                        help="Split each optimizer batch into this many groups per forward/backward pass. "
                             "Use 0 to disable gradient accumulation.")
    parser.add_argument("--hidden", default="256",
                        help="Hidden architecture: N for N,N or H1xH2 for two different hidden layers.")
    parser.add_argument("--hidden2", type=int, default=None)
    parser.add_argument("--lr", type=float, default=2e-3)
    parser.add_argument("--weight-decay", type=float, default=1e-5)
    parser.add_argument("--label-smoothing", type=float, default=0.0,
                        help="Cross-entropy label smoothing for hard top-1 teacher labels.")
    parser.add_argument("--quiet-weight", type=float, default=1.0,
                        help="Per-group training multiplier for quiet target moves.")
    parser.add_argument("--capture-weight", type=float, default=1.0,
                        help="Per-group training multiplier for capture target moves.")
    parser.add_argument("--check-weight", type=float, default=1.0,
                        help="Per-group training multiplier for checking target moves.")
    parser.add_argument("--promotion-weight", type=float, default=1.0,
                        help="Per-group training multiplier for promotion target moves.")
    parser.add_argument("--hard-examples", default="",
                        help="Optional .npy array of global group ids to upweight during training.")
    parser.add_argument("--hard-example-weight", type=float, default=1.0)
    parser.add_argument("--val-fraction", type=float, default=0.02)
    parser.add_argument("--val-groups", type=int, default=0,
                        help="Absolute validation group count. Overrides --val-fraction when > 0.")
    parser.add_argument("--max-val-groups", type=int, default=200000,
                        help="Cap validation size so large datasets do not spend epochs evaluating.")
    parser.add_argument("--train-eval-groups", type=int, default=50000,
                        help="Evaluate train metrics on this many groups. Use 0 for the full train set.")
    parser.add_argument("--bucket-eval-groups", type=int, default=50000,
                        help="Maximum validation groups per final tactical bucket. Use 0 for all.")
    parser.add_argument("--epoch-groups", type=int, default=0,
                        help="Train on at most this many groups per epoch. Use 0 for a full epoch.")
    parser.add_argument("--shuffle", choices=["blocks", "groups", "none"], default="blocks",
                        help="Block shuffle preserves disk locality for memory-mapped datasets.")
    parser.add_argument("--contiguous-read-overfetch", type=float, default=1.15,
                        help="Read one contiguous feature span when it overfetches by no more than this ratio.")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--torch-threads", type=int, default=int(os.environ.get("TORCH_NUM_THREADS", "0") or "0"))
    parser.add_argument("--mmap", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--amp", choices=["auto", "on", "off"], default="auto")
    parser.add_argument("--compile", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--checkpoint-every-epoch", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--resume", nargs="?", const="latest", default="",
                        help="Resume from a checkpoint path, or from latest in --out-dir when passed without a value.")
    parser.add_argument("--save-misses", action=argparse.BooleanOptionalAction, default=True,
                        help="Save train-eval and validation top-1 misses as .npy files for hard-example mining.")
    parser.add_argument("--progress-every", type=int, default=0,
                        help="Print a training progress line every N optimizer steps.")
    args = parser.parse_args()
    if args.label_smoothing < 0.0 or args.label_smoothing >= 1.0:
        raise RuntimeError("--label-smoothing must be in [0, 1)")
    for name in ("quiet_weight", "capture_weight", "check_weight", "promotion_weight", "hard_example_weight"):
        if getattr(args, name) <= 0.0:
            raise RuntimeError(f"--{name.replace('_', '-')} must be positive")

    if args.torch_threads > 0:
        torch.set_num_threads(args.torch_threads)
    try:
        torch.set_float32_matmul_precision("high")
    except Exception:
        pass

    hidden1, hidden2 = parse_hidden(args.hidden, args.hidden2)
    np.random.seed(args.seed)
    torch.manual_seed(args.seed)

    dataset = load_dataset(args.data, args.mmap)
    if dataset.groups <= 0:
        raise RuntimeError("no groups in dataset")
    if len(dataset.offsets) != dataset.groups + 1:
        raise RuntimeError("dataset has inconsistent group_offsets/target_indices lengths")

    train_groups, val_groups = split_groups(dataset.groups, args)
    train_eval_groups = sample_groups(train_groups, args.train_eval_groups, args.seed + 1009)
    val_eval_groups = val_groups
    group_weights = build_group_weights(dataset, args)

    device = resolve_device(args.device)
    use_amp = args.amp == "on" or (args.amp == "auto" and device.type == "cuda")
    raw_model = SmokePolicy(dataset.feature_dim, hidden1, hidden2).to(device)
    model = torch.compile(raw_model) if args.compile else raw_model
    optimizer = torch.optim.AdamW(raw_model.parameters(), lr=args.lr, weight_decay=args.weight_decay)
    scaler = None
    if use_amp and device.type == "cuda":
        try:
            scaler = torch.amp.GradScaler("cuda")
        except (AttributeError, TypeError):
            scaler = torch.cuda.amp.GradScaler()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    start_epoch = 1
    history: list[dict] = []
    resume_path: Path | None = None
    if args.resume:
        resume_path = latest_checkpoint(args.out_dir) if args.resume == "latest" else Path(args.resume)
        if resume_path is None or not resume_path.exists():
            raise RuntimeError(f"resume checkpoint not found: {args.resume}")
        checkpoint = torch.load(resume_path, map_location=device, weights_only=False)
        raw_model.load_state_dict(checkpoint["model"])
        optimizer.load_state_dict(checkpoint["optimizer"])
        if scaler is not None and checkpoint.get("scaler") is not None:
            scaler.load_state_dict(checkpoint["scaler"])
        history = list(checkpoint.get("history") or [])
        start_epoch = int(checkpoint["epoch"]) + 1

    config = {
        "data": str(args.data),
        "dataset_mmap": bool(dataset.mmap),
        "positions": dataset.groups,
        "candidates": dataset.candidates,
        "feature_dim": dataset.feature_dim,
        "feature_storage_dtype": str(dataset.features.dtype),
        "feature_layout": dataset.meta.get("feature_layout"),
        "hidden1": hidden1,
        "hidden2": hidden2,
        "batch_groups": args.batch_groups,
        "micro_batch_groups": args.micro_batch_groups,
        "epoch_groups": args.epoch_groups,
        "train_groups": int(len(train_groups)),
        "val_groups": int(len(val_groups)),
        "train_eval_groups": int(len(train_eval_groups)),
        "val_eval_groups": int(len(val_eval_groups)),
        "shuffle": args.shuffle,
        "device": str(device),
        "amp": use_amp,
        "compile": args.compile,
        "label_smoothing": args.label_smoothing,
        "quiet_weight": args.quiet_weight,
        "capture_weight": args.capture_weight,
        "check_weight": args.check_weight,
        "promotion_weight": args.promotion_weight,
        "hard_examples": args.hard_examples,
        "hard_example_weight": args.hard_example_weight,
        "weighted_training": group_weights is not None,
        "resume": str(resume_path) if resume_path is not None else "",
        "torch_threads": args.torch_threads,
        "meta": dataset.meta,
    }
    (args.out_dir / "config.json").write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")

    print(json.dumps({"config": config}, separators=(",", ":")))
    if start_epoch > args.epochs:
        print(f"resume checkpoint already reached epoch {start_epoch - 1}; writing final artifacts", file=sys.stderr)
    for epoch in range(start_epoch, args.epochs + 1):
        model.train()
        epoch_rng = np.random.default_rng(args.seed + epoch)
        total_loss = 0.0
        total_groups = 0
        total_candidates = 0
        overfetched = 0
        steps = 0
        start_time = time.perf_counter()

        for batch_group_ids in iter_group_batches(
            train_groups,
            args.batch_groups,
            args.shuffle,
            epoch_rng,
            args.epoch_groups,
        ):
            optimizer.zero_grad(set_to_none=True)
            logical_groups = len(batch_group_ids)
            micro_size = args.micro_batch_groups if args.micro_batch_groups > 0 else logical_groups
            micro_size = max(1, min(micro_size, logical_groups))

            batch_loss_sum = 0.0
            batch_candidates = 0
            batch_overfetched = 0
            for micro_start in range(0, logical_groups, micro_size):
                micro_group_ids = batch_group_ids[micro_start:micro_start + micro_size]
                batch = prepare_batch(dataset, micro_group_ids, args.contiguous_read_overfetch, group_weights)
                grouped, target_cols, weights = grouped_logits(model, batch, device, use_amp)
                loss = weighted_cross_entropy(grouped, target_cols, weights, args.label_smoothing)
                weighted_loss = loss * (len(micro_group_ids) / logical_groups)
                if scaler is not None:
                    scaler.scale(weighted_loss).backward()
                else:
                    weighted_loss.backward()
                batch_loss_sum += float(loss.item()) * len(micro_group_ids)
                batch_candidates += batch.candidates
                batch_overfetched += batch.overfetched

            if scaler is not None:
                scaler.step(optimizer)
                scaler.update()
            else:
                optimizer.step()

            steps += 1
            total_loss += batch_loss_sum
            total_groups += logical_groups
            total_candidates += batch_candidates
            overfetched += batch_overfetched
            if args.progress_every > 0 and steps % args.progress_every == 0:
                elapsed = max(1e-9, time.perf_counter() - start_time)
                print(
                    f"epoch {epoch} step {steps} groups {total_groups} "
                    f"loss {total_loss / max(1, total_groups):.6f} "
                    f"groups_per_sec {total_groups / elapsed:.1f}",
                    file=sys.stderr,
                    flush=True,
                )

        elapsed = max(1e-9, time.perf_counter() - start_time)
        train_metrics = evaluate(model, dataset, train_eval_groups, args.batch_groups, device, args, use_amp)
        val_metrics = evaluate(model, dataset, val_eval_groups, args.batch_groups, device, args, use_amp)
        epoch_record = {
            "epoch": epoch,
            "mean_train_loss": total_loss / max(1, total_groups),
            "trained_groups": int(total_groups),
            "trained_candidates": int(total_candidates),
            "overfetched_candidates": int(overfetched),
            "seconds": elapsed,
            "groups_per_second": total_groups / elapsed,
            "train": train_metrics,
            "val": val_metrics,
        }
        history.append(epoch_record)
        print(json.dumps(epoch_record, separators=(",", ":")), flush=True)
        if args.checkpoint_every_epoch:
            save_training_checkpoint(
                args.out_dir / f"checkpoint-epoch{epoch:03d}.pt",
                raw_model,
                optimizer,
                scaler,
                epoch,
                history,
                dataset,
                hidden1,
                hidden2,
                config,
            )

    if not history:
        raise RuntimeError("no epochs were trained")

    if dataset.bucket_flags is not None and len(val_eval_groups) > 0:
        history[-1]["val_buckets"] = evaluate_buckets(
            model, dataset, val_eval_groups, args.batch_groups, device, args, use_amp
        )
        print(json.dumps({"final_val_buckets": history[-1]["val_buckets"]}, separators=(",", ":")), flush=True)

    if args.save_misses:
        train_misses = collect_misses(model, dataset, train_eval_groups, args.batch_groups, device, args, use_amp)
        val_misses = collect_misses(model, dataset, val_eval_groups, args.batch_groups, device, args, use_amp)
        np.save(args.out_dir / "train_eval_misses.npy", train_misses)
        np.save(args.out_dir / "val_misses.npy", val_misses)
        history[-1]["misses"] = {
            "train_eval_misses": int(len(train_misses)),
            "val_misses": int(len(val_misses)),
            "train_eval_misses_path": str(args.out_dir / "train_eval_misses.npy"),
            "val_misses_path": str(args.out_dir / "val_misses.npy"),
        }
        print(json.dumps({"misses": history[-1]["misses"]}, separators=(",", ":")), flush=True)

    model_path = args.out_dir / "smoke-policy.pt"
    torch.save({
        "model": raw_model.state_dict(),
        "feature_dim": dataset.feature_dim,
        "hidden": hidden1 if hidden1 == hidden2 else None,
        "hidden1": hidden1,
        "hidden2": hidden2,
        "meta": dataset.meta,
        "config": config,
    }, model_path)

    metrics = {
        "data": str(args.data),
        "model": str(model_path),
        "positions": dataset.groups,
        "candidates": dataset.candidates,
        "feature_dim": dataset.feature_dim,
        "hidden1": hidden1,
        "hidden2": hidden2,
        "train_groups": int(len(train_groups)),
        "val_groups": int(len(val_groups)),
        "history": history,
        "final": history[-1],
        "config": config,
    }
    metrics_path = args.out_dir / "metrics.json"
    metrics_path.write_text(json.dumps(metrics, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {metrics_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
