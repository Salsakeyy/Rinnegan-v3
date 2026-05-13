#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

PYTHON="${PYTHON:-python3}"
OUT="${OUT:-data/policy/fast-policy}"
LABELS="${LABELS:-}"
DATASET="${DATASET:-}"
LIMIT="${LIMIT:-0}"
ARCHES="${ARCHES:-384 512}"
FEATURE_LAYOUT="${FEATURE_LAYOUT:-legacy}"
FEATURE_DIM="${FEATURE_DIM:-32}"
FEATURE_DTYPE="${FEATURE_DTYPE:-f32}"
QUIET_ONLY="${QUIET_ONLY:-0}"
PACKER="${PACKER:-auto}"
CPP_PACKER="${CPP_PACKER:-build/pack_policy_dataset_cpp}"
DATASET_FORMAT="${DATASET_FORMAT:-npy-dir}"
WRITE_LABELS="${WRITE_LABELS:-0}"
WRITE_SIDECARS="${WRITE_SIDECARS:-0}"
MATERIALIZE_NPZ="${MATERIALIZE_NPZ:-1}"
MATERIALIZED_DATASET="${MATERIALIZED_DATASET:-$OUT/dataset-npy}"
PACK_PROGRESS_EVERY="${PACK_PROGRESS_EVERY:-100000}"
EPOCHS="${EPOCHS:-3}"
BATCH_GROUPS="${BATCH_GROUPS:-4096}"
MICRO_BATCH_GROUPS="${MICRO_BATCH_GROUPS:-0}"
EPOCH_GROUPS="${EPOCH_GROUPS:-2000000}"
VAL_FRACTION="${VAL_FRACTION:-0.02}"
MAX_VAL_GROUPS="${MAX_VAL_GROUPS:-200000}"
TRAIN_EVAL_GROUPS="${TRAIN_EVAL_GROUPS:-50000}"
BUCKET_EVAL_GROUPS="${BUCKET_EVAL_GROUPS:-50000}"
LR="${LR:-0.002}"
WEIGHT_DECAY="${WEIGHT_DECAY:-0.00001}"
LABEL_SMOOTHING="${LABEL_SMOOTHING:-0}"
QUIET_WEIGHT="${QUIET_WEIGHT:-1}"
CAPTURE_WEIGHT="${CAPTURE_WEIGHT:-1}"
CHECK_WEIGHT="${CHECK_WEIGHT:-1}"
PROMOTION_WEIGHT="${PROMOTION_WEIGHT:-1}"
HARD_EXAMPLES="${HARD_EXAMPLES:-}"
HARD_EXAMPLE_WEIGHT="${HARD_EXAMPLE_WEIGHT:-1}"
RESUME="${RESUME:-0}"
DEVICE="${DEVICE:-auto}"
SHUFFLE="${SHUFFLE:-blocks}"
OVERFETCH="${OVERFETCH:-1.15}"
AMP="${AMP:-auto}"
COMPILE="${COMPILE:-0}"
PROGRESS_EVERY="${PROGRESS_EVERY:-100}"
SEED="${SEED:-1}"

detect_logical_cores() {
  sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4
}

TORCH_THREADS="${TORCH_THREADS:-$(detect_logical_cores)}"

mkdir -p "$OUT"

if [[ -z "$DATASET" ]]; then
  if [[ -z "$LABELS" ]]; then
    echo "set LABELS=/path/to/labels.jsonl or DATASET=/path/to/dataset" >&2
    exit 1
  fi
  if [[ ! -e "$LABELS" && ! -p "$LABELS" ]]; then
    echo "missing labels: $LABELS" >&2
    exit 1
  fi

  PACK_FROM_GZIP=0
  LABELS_FOR_PACK="$LABELS"
  if [[ "$LABELS" == *.gz ]]; then
    PACK_FROM_GZIP=1
    LABELS_FOR_PACK=/dev/stdin
  fi

  if [[ "$DATASET_FORMAT" == "npy-dir" ]]; then
    DATASET="$OUT/dataset"
  else
    DATASET="$OUT/dataset.npz"
  fi

  if [[ "$PACKER" == "cpp" || ( "$PACKER" == "auto" && -x "$CPP_PACKER" ) ]]; then
    PACK_CMD=("$CPP_PACKER" \
      --input "$LABELS_FOR_PACK" \
      --output "$DATASET" \
      --output-format "$DATASET_FORMAT" \
      --feature-dim "$FEATURE_DIM" \
      --feature-layout "$FEATURE_LAYOUT" \
      --feature-dtype "$FEATURE_DTYPE" \
      --progress-every "$PACK_PROGRESS_EVERY")
    if [[ "$WRITE_LABELS" == "0" || "$WRITE_LABELS" == "false" ]]; then
      PACK_CMD+=(--no-labels)
    fi
    if [[ "$WRITE_SIDECARS" == "0" || "$WRITE_SIDECARS" == "false" ]]; then
      PACK_CMD+=(--no-sidecars)
    fi
  else
    if [[ "$DATASET_FORMAT" != "npz" ]]; then
      echo "Python packer can only write npz; build $CPP_PACKER or set DATASET_FORMAT=npz" >&2
      exit 1
    fi
    PACK_CMD=("$PYTHON" tools/policy/pack_policy_dataset.py \
      --input "$LABELS_FOR_PACK" \
      --output "$DATASET" \
      --feature-dim "$FEATURE_DIM" \
      --feature-layout "$FEATURE_LAYOUT" \
      --no-compressed)
  fi
  if [[ "$LIMIT" -gt 0 ]]; then
    PACK_CMD+=(--limit "$LIMIT")
  fi
  if [[ "$QUIET_ONLY" == "1" || "$QUIET_ONLY" == "true" ]]; then
    PACK_CMD+=(--quiet-only)
  fi
  if [[ "$PACK_FROM_GZIP" == "1" ]]; then
    "${PACK_CMD[@]}" < <(gzip -dc -- "$LABELS")
  else
    "${PACK_CMD[@]}"
  fi
fi

if [[ ! -e "$DATASET" ]]; then
  echo "missing dataset: $DATASET" >&2
  exit 1
fi

if [[ "$MATERIALIZE_NPZ" == "1" || "$MATERIALIZE_NPZ" == "true" ]]; then
  if [[ -f "$DATASET" && "$DATASET" == *.npz ]]; then
    if [[ ! -f "$MATERIALIZED_DATASET/features.npy" ]]; then
      "$PYTHON" tools/policy/materialize_policy_dataset.py \
        --input "$DATASET" \
        --output "$MATERIALIZED_DATASET" \
        --overwrite
    fi
    DATASET="$MATERIALIZED_DATASET"
  fi
fi

COMPILE_FLAG=--no-compile
if [[ "$COMPILE" == "1" || "$COMPILE" == "true" ]]; then
  COMPILE_FLAG=--compile
fi

RESUME_ARGS=()
if [[ "$RESUME" == "1" || "$RESUME" == "true" ]]; then
  RESUME_ARGS=(--resume)
elif [[ "$RESUME" != "0" && "$RESUME" != "false" && -n "$RESUME" ]]; then
  RESUME_ARGS=(--resume "$RESUME")
fi

HARD_EXAMPLE_ARGS=()
if [[ -n "$HARD_EXAMPLES" ]]; then
  HARD_EXAMPLE_ARGS=(--hard-examples "$HARD_EXAMPLES")
fi

ARCH_LIST="${ARCHES//,/ }"
read -r -a ARCH_ARRAY <<< "$ARCH_LIST"
if [[ "${#ARCH_ARRAY[@]}" -eq 0 ]]; then
  echo "ARCHES is empty" >&2
  exit 1
fi

for ARCH in "${ARCH_ARRAY[@]}"; do
  NAME="h${ARCH}"
  TRAIN_DIR="$OUT/$NAME/train"
  mkdir -p "$TRAIN_DIR"

  OMP_NUM_THREADS="$TORCH_THREADS" \
  MKL_NUM_THREADS="$TORCH_THREADS" \
  VECLIB_MAXIMUM_THREADS="$TORCH_THREADS" \
  TORCH_NUM_THREADS="$TORCH_THREADS" \
  "$PYTHON" tools/policy/train_smoke_policy.py \
    --data "$DATASET" \
    --out-dir "$TRAIN_DIR" \
    --epochs "$EPOCHS" \
    --batch-groups "$BATCH_GROUPS" \
    --micro-batch-groups "$MICRO_BATCH_GROUPS" \
    --hidden "$ARCH" \
    --lr "$LR" \
    --weight-decay "$WEIGHT_DECAY" \
    --label-smoothing "$LABEL_SMOOTHING" \
    --quiet-weight "$QUIET_WEIGHT" \
    --capture-weight "$CAPTURE_WEIGHT" \
    --check-weight "$CHECK_WEIGHT" \
    --promotion-weight "$PROMOTION_WEIGHT" \
    "${HARD_EXAMPLE_ARGS[@]}" \
    --hard-example-weight "$HARD_EXAMPLE_WEIGHT" \
    --val-fraction "$VAL_FRACTION" \
    --max-val-groups "$MAX_VAL_GROUPS" \
    --train-eval-groups "$TRAIN_EVAL_GROUPS" \
    --bucket-eval-groups "$BUCKET_EVAL_GROUPS" \
    --epoch-groups "$EPOCH_GROUPS" \
    --shuffle "$SHUFFLE" \
    --contiguous-read-overfetch "$OVERFETCH" \
    --device "$DEVICE" \
    --torch-threads "$TORCH_THREADS" \
    --amp "$AMP" \
    "$COMPILE_FLAG" \
    "${RESUME_ARGS[@]}" \
    --progress-every "$PROGRESS_EVERY" \
    --seed "$SEED"

  "$PYTHON" tools/policy/export_policy.py \
    --checkpoint "$TRAIN_DIR/smoke-policy.pt" \
    --output "$TRAIN_DIR/smoke-policy.bin"
done

"$PYTHON" - "$OUT" "${ARCH_ARRAY[@]}" <<'PY'
import json
import sys
from pathlib import Path

out = Path(sys.argv[1])
arches = sys.argv[2:]
rows = {}
for arch in arches:
    metrics_path = out / f"h{arch}" / "train" / "metrics.json"
    metrics = json.loads(metrics_path.read_text())
    final = metrics["final"]
    rows[arch] = {
        "positions": metrics["positions"],
        "candidates": metrics["candidates"],
        "feature_layout": metrics["config"].get("feature_layout"),
        "feature_dim": metrics["feature_dim"],
        "hidden1": metrics["hidden1"],
        "hidden2": metrics["hidden2"],
        "trained_groups": final["trained_groups"],
        "groups_per_second": final["groups_per_second"],
        "train_top1": final["train"]["top1"],
        "val_top1": final["val"]["top1"],
        "val_top3": final["val"]["top3"],
        "val_loss": final["val"]["loss"],
        "quiet_top1": (final.get("val_buckets") or {}).get("quiet", {}).get("top1"),
        "binary": str(out / f"h{arch}" / "train" / "smoke-policy.bin"),
    }

(out / "fast_train_summary.json").write_text(json.dumps(rows, indent=2) + "\n")
lines = [
    "| layout | arch | positions | candidates | trained groups | groups/s | train top1 | val top1 | quiet top1 | val top3 | val loss |",
    "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
]
for arch, row in rows.items():
    h = f"{row['hidden1']}x{row['hidden2']}"
    quiet_top1 = row["quiet_top1"]
    quiet_text = "" if quiet_top1 is None else f"{quiet_top1:.6f}"
    lines.append(
        f"| {row['feature_layout'] or 'unknown'}-{row['feature_dim']} | {h} | {row['positions']} | {row['candidates']} | {row['trained_groups']} | "
        f"{row['groups_per_second']:.1f} | {row['train_top1']:.6f} | "
        f"{row['val_top1']:.6f} | {quiet_text} | {row['val_top3']:.6f} | {row['val_loss']:.6f} |"
    )
(out / "fast_train_summary.md").write_text("\n".join(lines) + "\n")
print("\n".join(lines))
PY

echo "summary: $OUT/fast_train_summary.md"
