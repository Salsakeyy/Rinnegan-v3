#!/usr/bin/env python3
"""Train and export a Rinnegan-compatible raw NNUE file.

The model matches src/nnue.cpp:
  input features: 768 piece-square features
  hidden width:   768
  activation:     SCReLU clamp [0, 181], square
  output:         side-to-move accumulator first, opponent accumulator second
  export:         raw little-endian int16 arrays accepted by NNUE::load()
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import sys
from dataclasses import dataclass
from pathlib import Path

try:
    import chess
    import numpy as np
    import torch
    import torch.nn.functional as F
except ImportError as exc:  # pragma: no cover - dependency guard
    raise SystemExit(
        "Missing dependency. Install with "
        "`python3 -m pip install -r tools/nnue/requirements.txt`."
    ) from exc


ROOT = Path(__file__).resolve().parents[2]

INPUT = 768
HIDDEN = 768
QA = 181.0
QB = 64.0
SCALE = 400.0
QAB = QA * QB
PAD_IDX = INPUT


@dataclass
class EncodedData:
    white_idx: torch.Tensor
    black_idx: torch.Tensor
    stm: torch.Tensor
    target: torch.Tensor


def feature_indices(board: chess.Board) -> tuple[list[int], list[int]]:
    white: list[int] = []
    black: list[int] = []
    for sq, piece in board.piece_map().items():
        piece_type = piece.piece_type - 1
        if piece.color == chess.WHITE:
            white.append(64 * piece_type + sq)
            black.append(384 + 64 * piece_type + (sq ^ 56))
        else:
            white.append(384 + 64 * piece_type + sq)
            black.append(64 * piece_type + (sq ^ 56))
    return white, black


def load_records(path: Path, limit: int | None, clip_cp: int) -> list[tuple[str, float]]:
    records: list[tuple[str, float]] = []
    with path.open(encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            if limit is not None and len(records) >= limit:
                break
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            fen = record.get("fen")
            score = record.get("score_cp")
            if not isinstance(fen, str) or not isinstance(score, (int, float)):
                continue
            score = max(-clip_cp, min(clip_cp, float(score)))
            records.append((fen, score))
    return records


def encode_records(records: list[tuple[str, float]]) -> EncodedData:
    white_features: list[list[int]] = []
    black_features: list[list[int]] = []
    stm_values: list[int] = []
    targets: list[float] = []
    max_active = 0

    for fen, score in records:
        try:
            board = chess.Board(fen)
        except ValueError:
            continue
        white, black = feature_indices(board)
        max_active = max(max_active, len(white), len(black))
        white_features.append(white)
        black_features.append(black)
        stm_values.append(0 if board.turn == chess.WHITE else 1)
        targets.append(score)

    if not targets:
        raise RuntimeError("No trainable records were loaded")

    max_active = max(1, max_active)
    white_idx = torch.full((len(targets), max_active), PAD_IDX, dtype=torch.long)
    black_idx = torch.full((len(targets), max_active), PAD_IDX, dtype=torch.long)
    for row, features in enumerate(white_features):
        white_idx[row, : len(features)] = torch.tensor(features, dtype=torch.long)
    for row, features in enumerate(black_features):
        black_idx[row, : len(features)] = torch.tensor(features, dtype=torch.long)

    return EncodedData(
        white_idx=white_idx,
        black_idx=black_idx,
        stm=torch.tensor(stm_values, dtype=torch.long),
        target=torch.tensor(targets, dtype=torch.float32),
    )


class RinneganNnue(torch.nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.feature_weights = torch.nn.Parameter(torch.empty(INPUT, HIDDEN))
        self.feature_bias = torch.nn.Parameter(torch.zeros(HIDDEN))
        self.output_weights = torch.nn.Parameter(torch.empty(2 * HIDDEN))
        self.output_bias = torch.nn.Parameter(torch.zeros(()))
        self.register_buffer("pad_row", torch.zeros(1, HIDDEN), persistent=False)
        self.reset_parameters()

    def reset_parameters(self) -> None:
        torch.nn.init.normal_(self.feature_weights, mean=0.0, std=4.0)
        torch.nn.init.normal_(self.output_weights, mean=0.0, std=1.0)
        torch.nn.init.zeros_(self.feature_bias)
        torch.nn.init.zeros_(self.output_bias)

    def accumulator(self, indices: torch.Tensor) -> torch.Tensor:
        weights = torch.cat((self.feature_weights, self.pad_row), dim=0)
        flat = indices.reshape(-1)
        gathered = weights.index_select(0, flat).reshape(indices.shape[0], indices.shape[1], HIDDEN)
        return gathered.sum(dim=1) + self.feature_bias

    def forward(self, white_idx: torch.Tensor, black_idx: torch.Tensor, stm: torch.Tensor) -> torch.Tensor:
        white_acc = self.accumulator(white_idx)
        black_acc = self.accumulator(black_idx)
        white_to_move = (stm == 0).unsqueeze(1)
        us = torch.where(white_to_move, white_acc, black_acc)
        them = torch.where(white_to_move, black_acc, white_acc)

        us_act = torch.clamp(us, 0.0, QA).square()
        them_act = torch.clamp(them, 0.0, QA).square()
        raw = (
            us_act.matmul(self.output_weights[:HIDDEN])
            + them_act.matmul(self.output_weights[HIDDEN:])
        ) / QA + self.output_bias
        return raw * SCALE / QAB

    @torch.no_grad()
    def clamp_exportable(self) -> None:
        self.feature_weights.clamp_(-32768, 32767)
        self.feature_bias.clamp_(-32768, 32767)
        self.output_weights.clamp_(-32768, 32767)
        self.output_bias.clamp_(-32768, 32767)


def choose_device(requested: str) -> torch.device:
    if requested != "auto":
        return torch.device(requested)
    if torch.cuda.is_available():
        return torch.device("cuda")
    if hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
        return torch.device("mps")
    return torch.device("cpu")


def load_raw_net(model: RinneganNnue, path: Path) -> None:
    expected = INPUT * HIDDEN + HIDDEN + 2 * HIDDEN + 1
    arr = np.fromfile(path, dtype="<i2")
    if arr.size < expected:
        raise RuntimeError(f"{path} is too small for Rinnegan raw NNUE format")
    offset = 0
    fw = arr[offset : offset + INPUT * HIDDEN].reshape(INPUT, HIDDEN)
    offset += INPUT * HIDDEN
    fb = arr[offset : offset + HIDDEN]
    offset += HIDDEN
    ow = arr[offset : offset + 2 * HIDDEN]
    offset += 2 * HIDDEN
    ob = arr[offset]
    with torch.no_grad():
        model.feature_weights.copy_(torch.from_numpy(fw.astype(np.float32)))
        model.feature_bias.copy_(torch.from_numpy(fb.astype(np.float32)))
        model.output_weights.copy_(torch.from_numpy(ow.astype(np.float32)))
        model.output_bias.copy_(torch.tensor(float(ob)))


def to_i16_bytes(tensor: torch.Tensor) -> bytes:
    arr = (
        tensor.detach()
        .cpu()
        .round()
        .clamp(-32768, 32767)
        .to(torch.int16)
        .numpy()
        .astype("<i2", copy=False)
    )
    return arr.tobytes(order="C")


def export_raw_net(model: RinneganNnue, path: Path, pad64: bool) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    model.clamp_exportable()
    with path.open("wb") as handle:
        handle.write(to_i16_bytes(model.feature_weights))
        handle.write(to_i16_bytes(model.feature_bias))
        handle.write(to_i16_bytes(model.output_weights))
        handle.write(to_i16_bytes(model.output_bias.reshape(1)))
        if pad64:
            extra = (-handle.tell()) % 64
            if extra:
                handle.write(b"\0" * extra)


def move_data(data: EncodedData, device: torch.device) -> EncodedData:
    return EncodedData(
        white_idx=data.white_idx.to(device),
        black_idx=data.black_idx.to(device),
        stm=data.stm.to(device),
        target=data.target.to(device),
    )


def take(data: EncodedData, idx: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor, torch.Tensor]:
    return data.white_idx[idx], data.black_idx[idx], data.stm[idx], data.target[idx]


@torch.no_grad()
def evaluate(model: RinneganNnue, data: EncodedData, idx: torch.Tensor, batch_size: int) -> dict[str, float]:
    model.eval()
    total_abs = 0.0
    total_sq = 0.0
    total = 0
    for start in range(0, idx.numel(), batch_size):
        batch_idx = idx[start : start + batch_size]
        white, black, stm, target = take(data, batch_idx)
        pred = model(white, black, stm)
        diff = pred - target
        total_abs += diff.abs().sum().item()
        total_sq += diff.square().sum().item()
        total += target.numel()
    return {
        "mae": total_abs / max(1, total),
        "rmse": math.sqrt(total_sq / max(1, total)),
    }


def train(args: argparse.Namespace) -> dict[str, float]:
    random.seed(args.seed)
    torch.manual_seed(args.seed)
    if args.torch_threads > 0:
        torch.set_num_threads(args.torch_threads)

    records = load_records(args.data, args.limit, args.clip_cp)
    print(f"Loaded {len(records)} labeled positions", file=sys.stderr)
    encoded = encode_records(records)

    device = choose_device(args.device)
    data = move_data(encoded, device)
    model = RinneganNnue()
    if args.init_net:
        load_raw_net(model, args.init_net)
        print(f"Initialized from {args.init_net}", file=sys.stderr)
    model.to(device)

    n = data.target.numel()
    perm = torch.randperm(n, device=device)
    val_n = 0 if n < 100 else max(1, int(n * args.val_frac))
    val_idx = perm[:val_n]
    train_idx = perm[val_n:]

    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=args.weight_decay)
    best_val = float("inf")
    best_path = args.out.with_suffix(".best.net")

    for epoch in range(1, args.epochs + 1):
        model.train()
        order = train_idx[torch.randperm(train_idx.numel(), device=device)]
        total_loss = 0.0
        batches = 0
        for start in range(0, order.numel(), args.batch_size):
            batch_idx = order[start : start + args.batch_size]
            white, black, stm, target = take(data, batch_idx)
            pred = model(white, black, stm)
            loss = F.smooth_l1_loss(pred, target, beta=args.huber_beta)
            optimizer.zero_grad(set_to_none=True)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), args.grad_clip)
            optimizer.step()
            model.clamp_exportable()
            total_loss += loss.item()
            batches += 1

        train_metrics = evaluate(model, data, train_idx[: min(train_idx.numel(), args.eval_train_samples)], args.batch_size)
        val_metrics = evaluate(model, data, val_idx, args.batch_size) if val_n else train_metrics
        print(
            f"epoch={epoch} loss={total_loss / max(1, batches):.3f} "
            f"train_mae={train_metrics['mae']:.2f} val_mae={val_metrics['mae']:.2f} "
            f"val_rmse={val_metrics['rmse']:.2f}",
            file=sys.stderr,
            flush=True,
        )

        if val_metrics["mae"] < best_val:
            best_val = val_metrics["mae"]
            export_raw_net(model, best_path, args.pad64)

    export_raw_net(model, args.out, args.pad64)
    metrics = evaluate(model, data, val_idx if val_n else train_idx, args.batch_size)
    metrics["positions"] = float(n)
    return metrics


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, default=ROOT / "data" / "nnue" / "stockfish-depth40.jsonl")
    parser.add_argument("--out", type=Path, default=ROOT / "data" / "nnue" / "rinnegan-depth40.net")
    parser.add_argument("--init-net", type=Path, default=None)
    parser.add_argument("--limit", type=int)
    parser.add_argument("--epochs", type=int, default=8)
    parser.add_argument("--batch-size", type=int, default=4096)
    parser.add_argument("--lr", type=float, default=2e-3)
    parser.add_argument("--weight-decay", type=float, default=1e-5)
    parser.add_argument("--huber-beta", type=float, default=50.0)
    parser.add_argument("--grad-clip", type=float, default=10.0)
    parser.add_argument("--clip-cp", type=int, default=2000)
    parser.add_argument("--val-frac", type=float, default=0.02)
    parser.add_argument("--eval-train-samples", type=int, default=20_000)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--torch-threads", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--pad64", action=argparse.BooleanOptionalAction, default=True)
    args = parser.parse_args()

    metrics = train(args)
    metrics_path = args.out.with_suffix(".metrics.json")
    metrics_path.write_text(json.dumps(metrics, indent=2) + "\n", encoding="utf-8")
    print(f"Exported {args.out}")
    print(f"Metrics: {metrics}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
