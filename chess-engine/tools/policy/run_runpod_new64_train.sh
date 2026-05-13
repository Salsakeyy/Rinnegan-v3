#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

OUT="${OUT:-/workspace/rinnegan-policy/new64-10m-f32}"
LABELS="${LABELS:-/workspace/rinnegan-policy/data/rinnegan_depth8_labels.jsonl.gz}"
ARCHES="${ARCHES:-512 768x512 1024x512}"
EPOCHS="${EPOCHS:-3}"
BATCH_GROUPS="${BATCH_GROUPS:-4096}"
MICRO_BATCH_GROUPS="${MICRO_BATCH_GROUPS:-0}"
EPOCH_GROUPS="${EPOCH_GROUPS:-0}"
FEATURE_DTYPE="${FEATURE_DTYPE:-f32}"
LABEL_SMOOTHING="${LABEL_SMOOTHING:-0.03}"
QUIET_WEIGHT="${QUIET_WEIGHT:-1}"
HARD_EXAMPLES="${HARD_EXAMPLES:-}"
HARD_EXAMPLE_WEIGHT="${HARD_EXAMPLE_WEIGHT:-1}"
RESUME="${RESUME:-0}"
TRAIN_EVAL_GROUPS="${TRAIN_EVAL_GROUPS:-100000}"
BUCKET_EVAL_GROUPS="${BUCKET_EVAL_GROUPS:-100000}"
MAX_VAL_GROUPS="${MAX_VAL_GROUPS:-200000}"
TORCH_THREADS="${TORCH_THREADS:-$(nproc 2>/dev/null || echo 8)}"

if [[ ! -f "$LABELS" ]]; then
  echo "missing labels: $LABELS" >&2
  exit 1
fi

mkdir -p "$OUT"

echo "== system =="
python3 - <<'PY'
import torch
print("torch", torch.__version__)
print("cuda", torch.cuda.is_available())
if torch.cuda.is_available():
    print("gpu", torch.cuda.get_device_name(0))
    print("capability", torch.cuda.get_device_capability(0))
PY
df -h "$OUT" || true

echo "== build packer =="
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pack_policy_dataset_cpp -j "$TORCH_THREADS"

echo "== train =="
PYTHON=python3 \
OUT="$OUT" \
LABELS="$LABELS" \
FEATURE_LAYOUT=new64 \
FEATURE_DIM=64 \
FEATURE_DTYPE="$FEATURE_DTYPE" \
LABEL_SMOOTHING="$LABEL_SMOOTHING" \
QUIET_WEIGHT="$QUIET_WEIGHT" \
HARD_EXAMPLES="$HARD_EXAMPLES" \
HARD_EXAMPLE_WEIGHT="$HARD_EXAMPLE_WEIGHT" \
RESUME="$RESUME" \
WRITE_LABELS=0 \
WRITE_SIDECARS=0 \
DATASET_FORMAT=npy-dir \
PACKER=cpp \
ARCHES="$ARCHES" \
EPOCHS="$EPOCHS" \
EPOCH_GROUPS="$EPOCH_GROUPS" \
BATCH_GROUPS="$BATCH_GROUPS" \
MICRO_BATCH_GROUPS="$MICRO_BATCH_GROUPS" \
VAL_FRACTION=0.02 \
MAX_VAL_GROUPS="$MAX_VAL_GROUPS" \
TRAIN_EVAL_GROUPS="$TRAIN_EVAL_GROUPS" \
BUCKET_EVAL_GROUPS="$BUCKET_EVAL_GROUPS" \
DEVICE=cuda \
TORCH_THREADS="$TORCH_THREADS" \
OVERFETCH=1.15 \
AMP=auto \
COMPILE=0 \
PROGRESS_EVERY=100 \
PACK_PROGRESS_EVERY=250000 \
bash tools/policy/run_fast_policy_train.sh
