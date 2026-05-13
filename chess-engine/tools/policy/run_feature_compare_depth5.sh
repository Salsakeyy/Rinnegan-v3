#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

PYTHON="${PYTHON:-python3}"
COUNT="${COUNT:-1000000}"
DEPTH="${DEPTH:-5}"
OUT="${OUT:-data/policy/rinnegan-depth${DEPTH}-n${COUNT}-old32-vs-new32}"
ENGINE="${ENGINE:-build/engine}"
TEACHER_PROFILE="${TEACHER_PROFILE:-rinnegan}"
HASH="${HASH:-128}"
EPOCHS="${EPOCHS:-4}"
BATCH_GROUPS="${BATCH_GROUPS:-1024}"
HIDDEN="${HIDDEN:-256}"
DEVICE="${DEVICE:-cpu}"
SEED="${SEED:-1}"
PLY_BUCKETS="${PLY_BUCKETS:-8-16:0.35,17-40:0.40,41-80:0.20,81-120:0.05}"
TACTICAL_RATE="${TACTICAL_RATE:-0.15}"
SOURCE_FENS="${SOURCE_FENS:-$OUT/fens.txt}"
QUIET_ONLY="${QUIET_ONLY:-0}"
PACKER="${PACKER:-auto}"
CPP_PACKER="${CPP_PACKER:-build/pack_policy_dataset_cpp}"
PACK_PROGRESS_EVERY="${PACK_PROGRESS_EVERY:-50000}"

detect_logical_cores() {
  sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4
}

LOGICAL_CORES="${LOGICAL_CORES:-$(detect_logical_cores)}"
HALF_CORES=$(( LOGICAL_CORES / 2 ))
if [[ "$HALF_CORES" -lt 1 ]]; then
  HALF_CORES=1
fi
THREADS="${THREADS:-$HALF_CORES}"
TORCH_THREADS="${TORCH_THREADS:-$THREADS}"

line_count() {
  if [[ -f "$1" ]]; then
    wc -l < "$1" | tr -d ' '
  else
    echo 0
  fi
}

if [[ ! -x "$ENGINE" ]]; then
  echo "missing executable engine: $ENGINE" >&2
  echo "build it first with: cmake --build build --target engine" >&2
  exit 1
fi

mkdir -p "$OUT"

FEN_COUNT="$(line_count "$SOURCE_FENS")"
if [[ "$FEN_COUNT" -lt "$COUNT" ]]; then
  if [[ "$SOURCE_FENS" != "$OUT/fens.txt" && "$FEN_COUNT" -gt 0 ]]; then
    echo "source FEN file has only $FEN_COUNT rows, need $COUNT: $SOURCE_FENS" >&2
    exit 1
  fi
  "$PYTHON" tools/policy/generate_fens.py \
    --count "$COUNT" \
    --openings tests/openings.epd \
    --include-openings \
    --min-ply 8 \
    --max-ply 120 \
    --ply-buckets "$PLY_BUCKETS" \
    --tactical-rate "$TACTICAL_RATE" \
    --seed "$SEED" \
    --stream-output \
    --resume-output \
    --flush-every 1000 \
    --progress 10000 \
    --output "$SOURCE_FENS"
fi

LABELS="$OUT/labels.jsonl"
"$PYTHON" tools/policy/label_uci.py \
  --engine "$ENGINE" \
  --teacher-profile "$TEACHER_PROFILE" \
  --depth "$DEPTH" \
  --nodes 0 \
  --threads "$THREADS" \
  --hash "$HASH" \
  --input "$SOURCE_FENS" \
  --output "$LABELS" \
  --limit "$COUNT" \
  --resume \
  --flush-every 1000 \
  --option UsePolicy=false

for LAYOUT in legacy new32; do
  NAME="$LAYOUT"
  if [[ "$LAYOUT" == "legacy" ]]; then
    NAME="old32"
  fi
  DATASET="$OUT/$NAME/dataset.npz"
  TRAIN_DIR="$OUT/$NAME/train"
  if [[ "$PACKER" == "cpp" || ( "$PACKER" == "auto" && -x "$CPP_PACKER" ) ]]; then
    PACK_CMD=("$CPP_PACKER" \
      --input "$LABELS" \
      --output "$DATASET" \
      --feature-dim 32 \
      --feature-layout "$LAYOUT" \
      --progress-every "$PACK_PROGRESS_EVERY")
  else
    PACK_CMD=("$PYTHON" tools/policy/pack_policy_dataset.py \
      --input "$LABELS" \
      --output "$DATASET" \
      --feature-dim 32 \
      --feature-layout "$LAYOUT")
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
done

"$PYTHON" - "$OUT" <<'PY'
import json
import sys
from pathlib import Path

out = Path(sys.argv[1])
rows = {}
for name in ("old32", "new32"):
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
    }

delta = {
    "val_top1": rows["new32"]["val_top1"] - rows["old32"]["val_top1"],
    "val_top3": rows["new32"]["val_top3"] - rows["old32"]["val_top3"],
    "loss": rows["new32"]["loss"] - rows["old32"]["loss"],
}
payload = {"rows": rows, "delta_new32_minus_old32": delta}
(out / "comparison.json").write_text(json.dumps(payload, indent=2) + "\n")

lines = [
    "| layout | positions | candidates | avg cand | train top1 | train top3 | val top1 | val top3 | val loss |",
    "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
]
for name in ("old32", "new32"):
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
(out / "comparison.md").write_text("\n".join(lines) + "\n")
print("\n".join(lines))
PY

echo "comparison: $OUT/comparison.md"
