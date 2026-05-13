#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

PYTHON="${PYTHON:-python3}"
LABELS="${LABELS:?set LABELS=/path/to/labels.jsonl}"
OUT="${OUT:?set OUT=/path/to/output-dir}"
LIMIT="${LIMIT:-0}"
EPOCHS="${EPOCHS:-4}"
BATCH_GROUPS="${BATCH_GROUPS:-1024}"
HIDDEN="${HIDDEN:-256}"
DEVICE="${DEVICE:-cpu}"
TORCH_THREADS="${TORCH_THREADS:-4}"
QUIET_ONLY="${QUIET_ONLY:-0}"
PACKER="${PACKER:-auto}"
CPP_PACKER="${CPP_PACKER:-build/pack_policy_dataset_cpp}"
PACK_PROGRESS_EVERY="${PACK_PROGRESS_EVERY:-50000}"
SF_LABELS="${SF_LABELS:-}"

if [[ ! -f "$LABELS" ]]; then
  echo "missing labels: $LABELS" >&2
  exit 1
fi
if [[ -n "$SF_LABELS" && ! -f "$SF_LABELS" ]]; then
  echo "missing SF labels: $SF_LABELS" >&2
  exit 1
fi

mkdir -p "$OUT"

pack_and_train() {
  local NAME="$1"
  local LAYOUT="$2"
  local LABEL_FILE="$3"

  DATASET="$OUT/$NAME/dataset.npz"
  TRAIN_DIR="$OUT/$NAME/train"

  if [[ "$PACKER" == "cpp" || ( "$PACKER" == "auto" && -x "$CPP_PACKER" ) ]]; then
    PACK_CMD=("$CPP_PACKER" \
      --input "$LABEL_FILE" \
      --output "$DATASET" \
      --feature-dim 32 \
      --feature-layout "$LAYOUT" \
      --progress-every "$PACK_PROGRESS_EVERY")
  else
    PACK_CMD=("$PYTHON" tools/policy/pack_policy_dataset.py \
      --input "$LABEL_FILE" \
      --output "$DATASET" \
      --feature-dim 32 \
      --feature-layout "$LAYOUT")
  fi
  if [[ "$LIMIT" -gt 0 ]]; then
    PACK_CMD+=(--limit "$LIMIT")
  fi
  if [[ "$QUIET_ONLY" == "1" || "$QUIET_ONLY" == "true" ]]; then
    PACK_CMD+=(--quiet-only)
  fi
  "${PACK_CMD[@]}"

  OMP_NUM_THREADS="$TORCH_THREADS" \
  MKL_NUM_THREADS="$TORCH_THREADS" \
  VECLIB_MAXIMUM_THREADS="$TORCH_THREADS" \
  TORCH_NUM_THREADS="$TORCH_THREADS" \
  "$PYTHON" tools/policy/train_smoke_policy.py \
    --data "$DATASET" \
    --out-dir "$TRAIN_DIR" \
    --epochs "$EPOCHS" \
    --batch-groups "$BATCH_GROUPS" \
    --hidden "$HIDDEN" \
    --device "$DEVICE" \
    --torch-threads "$TORCH_THREADS"

  "$PYTHON" tools/policy/export_policy.py \
    --checkpoint "$TRAIN_DIR/smoke-policy.pt" \
    --output "$TRAIN_DIR/smoke-policy.bin"
}

pack_and_train old32 legacy "$LABELS"
pack_and_train new32 new32 "$LABELS"
if [[ -n "$SF_LABELS" ]]; then
  pack_and_train sf-new32 new32 "$SF_LABELS"
fi

"$PYTHON" - "$OUT" <<'PY'
import json
import sys
from pathlib import Path

out = Path(sys.argv[1])
rows = {}
names = ["old32", "new32"]
if (out / "sf-new32" / "train" / "metrics.json").exists():
    names.append("sf-new32")

for name in names:
    metrics = json.loads((out / name / "train" / "metrics.json").read_text())
    final = metrics["final"]
    rows[name] = {
        "positions": metrics["positions"],
        "candidates": metrics["candidates"],
        "avg_candidates": final["val"]["avg_candidates"],
        "train_top1": final["train"]["top1"],
        "train_top3": final["train"]["top3"],
        "val_top1": final["val"]["top1"],
        "val_top3": final["val"]["top3"],
        "loss": final["val"]["loss"],
        "val_buckets": final.get("val_buckets", {}),
    }

delta = {
    "val_top1": rows["new32"]["val_top1"] - rows["old32"]["val_top1"],
    "val_top3": rows["new32"]["val_top3"] - rows["old32"]["val_top3"],
    "loss": rows["new32"]["loss"] - rows["old32"]["loss"],
}
bucket_delta = {}
for bucket in ("quiet", "capture", "check", "promotion"):
    old = rows["old32"]["val_buckets"].get(bucket)
    new = rows["new32"]["val_buckets"].get(bucket)
    if old and new:
        bucket_delta[bucket] = {
            "top1": new["top1"] - old["top1"],
            "top3": new["top3"] - old["top3"],
            "loss": new["loss"] - old["loss"],
            "old_groups": old.get("groups", 0),
            "new_groups": new.get("groups", 0),
        }

payload = {
    "rows": rows,
    "delta_new32_minus_old32": delta,
    "bucket_delta_new32_minus_old32": bucket_delta,
}
(out / "comparison.json").write_text(json.dumps(payload, indent=2) + "\n")

lines = [
    "| layout | positions | candidates | avg cand | train top1 | train top3 | val top1 | val top3 | val loss |",
    "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
]
for name in names:
    row = rows[name]
    lines.append(
        f"| {name} | {row['positions']} | {row['candidates']} | "
        f"{row['avg_candidates']:.3f} | {row['train_top1']:.6f} | "
        f"{row['train_top3']:.6f} | {row['val_top1']:.6f} | "
        f"{row['val_top3']:.6f} | {row['loss']:.6f} |"
    )
lines.extend([
    "",
    f"new32 - old32 val top1: {delta['val_top1']:+.6f}",
    f"new32 - old32 val top3: {delta['val_top3']:+.6f}",
    f"new32 - old32 val loss: {delta['loss']:+.6f}",
])
if bucket_delta:
    lines.extend([
        "",
        "| bucket | old32 groups | old32 top1 | new32 top1 | delta top1 | old32 top3 | new32 top3 | delta top3 | delta loss |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ])
    for bucket in ("quiet", "capture", "check", "promotion"):
        if bucket not in bucket_delta:
            continue
        old = rows["old32"]["val_buckets"][bucket]
        new = rows["new32"]["val_buckets"][bucket]
        delta_row = bucket_delta[bucket]
        lines.append(
            f"| {bucket} | {old.get('groups', 0)} | {old['top1']:.6f} | {new['top1']:.6f} | "
            f"{delta_row['top1']:+.6f} | {old['top3']:.6f} | {new['top3']:.6f} | "
            f"{delta_row['top3']:+.6f} | {delta_row['loss']:+.6f} |"
        )

if "sf-new32" in rows and rows["sf-new32"]["val_buckets"]:
    lines.extend([
        "",
        "sf-new32 is trained/evaluated against Lichess eval Stockfish PV labels, not Rinnegan labels.",
        "",
        "| sf bucket | groups | top1 | top3 | loss |",
        "|---|---:|---:|---:|---:|",
    ])
    for bucket in ("quiet", "capture", "check", "promotion"):
        metrics = rows["sf-new32"]["val_buckets"].get(bucket)
        if metrics:
            lines.append(
                f"| {bucket} | {metrics.get('groups', 0)} | {metrics['top1']:.6f} | "
                f"{metrics['top3']:.6f} | {metrics['loss']:.6f} |"
            )
(out / "comparison.md").write_text("\n".join(lines) + "\n")
print("\n".join(lines))
PY

echo "comparison: $OUT/comparison.md"
