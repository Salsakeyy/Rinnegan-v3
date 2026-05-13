#!/usr/bin/env python3
"""Export the smoke policy checkpoint to a simple C++ runtime binary."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

import numpy as np
import torch


MAGIC = b"RINPOL1\0"
LAYOUT_IDS = {
    "legacy": 0,
    "new32": 1,
    "new64": 2,
}


def tensor(checkpoint: dict, name: str) -> np.ndarray:
    value = checkpoint["model"][name].detach().cpu().numpy().astype("<f4", copy=False)
    return np.ascontiguousarray(value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--layout", choices=sorted(LAYOUT_IDS), default=None,
                        help="Policy feature layout. Defaults to checkpoint/config metadata.")
    parser.add_argument("--format-version", type=int, choices=[1, 2], default=2,
                        help="Binary format version. Version 2 stores an explicit layout id.")
    args = parser.parse_args()

    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    feature_dim = int(checkpoint["feature_dim"])
    hidden1 = int(checkpoint.get("hidden1") or checkpoint.get("hidden") or checkpoint["model"]["net.0.bias"].numel())
    hidden2 = int(checkpoint.get("hidden2") or checkpoint.get("hidden") or checkpoint["model"]["net.2.bias"].numel())
    meta = checkpoint.get("meta") or {}
    config = checkpoint.get("config") or {}
    layout = args.layout or meta.get("feature_layout") or config.get("feature_layout")
    if layout is None:
        layout = "new32" if feature_dim == 32 else "legacy"
    layout = str(layout)
    expected_dim = {"legacy": feature_dim, "new32": 32, "new64": 64}[layout]
    if feature_dim != expected_dim:
        raise SystemExit(f"layout {layout} expects feature_dim {expected_dim}, got {feature_dim}")

    arrays = [
        tensor(checkpoint, "net.0.weight"),
        tensor(checkpoint, "net.0.bias"),
        tensor(checkpoint, "net.2.weight"),
        tensor(checkpoint, "net.2.bias"),
        tensor(checkpoint, "net.4.weight"),
        tensor(checkpoint, "net.4.bias"),
    ]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as handle:
        if args.format_version == 1:
            if layout == "new64":
                raise SystemExit("format version 1 cannot represent new64")
            handle.write(struct.pack("<8s4I", MAGIC, 1, feature_dim, hidden1, hidden2))
        else:
            handle.write(struct.pack("<8s5I", MAGIC, 2, LAYOUT_IDS[layout], feature_dim, hidden1, hidden2))
        for array in arrays:
            handle.write(array.tobytes(order="C"))

    print(f"exported {layout} policy {feature_dim}->{hidden1}->{hidden2}->1 to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
