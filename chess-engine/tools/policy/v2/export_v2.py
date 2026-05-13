#!/usr/bin/env python3
"""Export a v2 trained checkpoint to a RINPOL2 binary.

The runtime side (see `Policy::loadV2` in src/policy.cpp) accepts a
v1-shape MLP (`feature_dim -> h1 -> h2 -> 1`) plus per-bucket and
optional per-phase calibration vectors. The "trunk" architecture in
train_v2.py cannot be exported in this format yet because the engine
loader does not know about a separate ctx trunk.

For the concat architecture the export is straightforward: pack net.0
/ net.2 / net.4 weights, then bucket scale/bias and phase scale.

Bucket and phase calibration are optional. If you have not run the
calibration step, the defaults (scale=1.0, bias=0.0) are written so
the runtime path is identical to v1 plus the quiet_residual gating.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np
import torch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from schema import BUCKET_NAMES  # noqa: E402

MAGIC = b"RINPOL2\0"
LAYOUT_IDS = {"legacy": 0, "new32": 1, "new64": 2}
DEFAULT_BUCKETS = 4  # quiet, capture, check, promotion


def to_array(checkpoint: dict, key: str) -> np.ndarray:
    return np.ascontiguousarray(
        checkpoint["model"][key].detach().cpu().numpy().astype("<f4", copy=False)
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--bucket-scale", type=float, nargs="+", default=None,
                        help=f"Per-bucket multiplicative scale ({len(BUCKET_NAMES)} values). "
                             "Order: quiet capture check promotion.")
    parser.add_argument("--bucket-bias", type=float, nargs="+", default=None,
                        help=f"Per-bucket additive bias ({len(BUCKET_NAMES)} values).")
    parser.add_argument("--phase-scale", type=float, nargs="+", default=None,
                        help="Per-phase multiplicative scale (typically 3 values: opening / middlegame / endgame). "
                             "Pass empty / omit to disable phase calibration.")
    parser.add_argument("--calibration", type=Path, default=None,
                        help="Path to a calibration JSON produced by "
                             "tools/policy/v2/eval_v2.py --emit-calibration. "
                             "Fields bucket_scale / bucket_bias / phase_scale "
                             "are loaded and override the matching CLI flags "
                             "when those flags are not explicitly set.")
    args = parser.parse_args()

    # Calibration JSON merges into the argparse view: explicit CLI flags
    # still win, but otherwise the JSON's vectors are used. This keeps the
    # CLI surface intact while making the bucket/phase calibration path the
    # default deploy route.
    if args.calibration is not None:
        try:
            cal = json.loads(args.calibration.read_text())
        except Exception as exc:
            raise SystemExit(f"failed to read calibration JSON {args.calibration}: {exc}")
        if args.bucket_scale is None and "bucket_scale" in cal:
            args.bucket_scale = [float(x) for x in cal["bucket_scale"]]
        if args.bucket_bias  is None and "bucket_bias"  in cal:
            args.bucket_bias  = [float(x) for x in cal["bucket_bias"]]
        if not args.phase_scale and cal.get("phase_scale"):
            args.phase_scale  = [float(x) for x in cal["phase_scale"]]

    ckpt = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    config = ckpt.get("config", {})
    arch = config.get("arch", "concat")
    if arch != "concat":
        raise SystemExit(
            f"export_v2.py currently supports arch='concat' only (got {arch!r}). "
            "The runtime loader has no trunk support yet."
        )

    f_move = int(config["f_move"])
    f_ctx  = int(config["f_ctx"])
    h1     = int(config["hidden1"])
    h2     = int(config["hidden2"])
    feature_dim = f_move + f_ctx
    # Pull the feature layout from the dataset meta. The trainer stamps the
    # dataset's `feature_space` into the checkpoint via meta. new32_compat
    # → "new32" (matches src/policy.cpp::fillNew32FeaturesWithCtx).
    feature_space = (ckpt.get("meta") or {}).get("feature_space", "split")
    if feature_space == "new32_compat":
        layout = "new32"
    elif feature_space == "split" and feature_dim == 17:
        layout = "legacy"
    else:
        layout = "legacy"
    layout_id = LAYOUT_IDS[layout]

    # Engine validates layout vs feature_dim:
    #   legacy → 17 or 32; new32 → 32; new64 → 64.
    if (layout == "legacy" and feature_dim not in (17, 32)) or \
       (layout == "new32"  and feature_dim != 32) or \
       (layout == "new64"  and feature_dim != 64):
        print(f"warning: feature_dim={feature_dim} does not match layout={layout!r}; "
              "engine loader will reject this binary. Re-pack with --feature-space new32_compat "
              "or use a layout that matches your trained features.",
              file=sys.stderr)

    w1 = to_array(ckpt, "net.0.weight"); b1 = to_array(ckpt, "net.0.bias")
    w2 = to_array(ckpt, "net.2.weight"); b2 = to_array(ckpt, "net.2.bias")
    w3 = to_array(ckpt, "net.4.weight"); b3 = to_array(ckpt, "net.4.bias")

    bs = np.ones(DEFAULT_BUCKETS, dtype=np.float32)
    bb = np.zeros(DEFAULT_BUCKETS, dtype=np.float32)
    if args.bucket_scale is not None:
        if len(args.bucket_scale) != DEFAULT_BUCKETS:
            raise SystemExit(f"--bucket-scale needs {DEFAULT_BUCKETS} values")
        bs = np.asarray(args.bucket_scale, dtype="<f4")
    if args.bucket_bias is not None:
        if len(args.bucket_bias) != DEFAULT_BUCKETS:
            raise SystemExit(f"--bucket-bias needs {DEFAULT_BUCKETS} values")
        bb = np.asarray(args.bucket_bias, dtype="<f4")

    phases = 0
    ps = np.zeros(0, dtype="<f4")
    if args.phase_scale:
        phases = len(args.phase_scale)
        ps = np.asarray(args.phase_scale, dtype="<f4")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as h:
        # magic + version + layout + dims + bucket count + phase count
        h.write(struct.pack("<8s7I", MAGIC, 1, layout_id, feature_dim, h1, h2,
                            DEFAULT_BUCKETS, phases))
        h.write(w1.tobytes())
        h.write(b1.tobytes())
        h.write(w2.tobytes())
        h.write(b2.tobytes())
        h.write(w3.tobytes())
        h.write(np.asarray(b3, dtype="<f4").tobytes())
        h.write(bs.tobytes())
        h.write(bb.tobytes())
        if phases > 0:
            h.write(ps.tobytes())

    sidecar = args.output.with_suffix(args.output.suffix + ".meta.json")
    sidecar.write_text(json.dumps({
        "feature_dim": feature_dim, "f_move": f_move, "f_ctx": f_ctx,
        "hidden1": h1, "hidden2": h2,
        "buckets": DEFAULT_BUCKETS, "bucket_names": list(BUCKET_NAMES),
        "phases": phases,
        "bucket_scale": bs.tolist(), "bucket_bias": bb.tolist(),
        "phase_scale": ps.tolist(),
    }, indent=2))
    print(f"exported RINPOL2 ({feature_dim}->{h1}->{h2}->1, {DEFAULT_BUCKETS} buckets, "
          f"{phases} phases) to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
