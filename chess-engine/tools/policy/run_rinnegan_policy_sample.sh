#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

SOURCE_FENS="${SOURCE_FENS:-data/policy/source-fens.txt}"
OUT="${OUT:-data/policy/rinnegan-nodes500-n250k-quiet}"
LIMIT="${LIMIT:-250000}"
NODES="${NODES:-500}"
EPOCHS="${EPOCHS:-4}"
BATCH_GROUPS="${BATCH_GROUPS:-1024}"
HIDDEN="${HIDDEN:-256}"
ENGINE="${ENGINE:-build/engine}"
TEACHER_PROFILE="${TEACHER_PROFILE:-rinnegan}"
THREADS="${THREADS:-1}"
HASH="${HASH:-16}"
TORCH_THREADS="${TORCH_THREADS:-4}"
DEVICE="${DEVICE:-cpu}"

if [[ ! -x "$ENGINE" ]]; then
  echo "missing executable engine: $ENGINE" >&2
  echo "build it first with: cmake --build build --target engine" >&2
  exit 1
fi
if [[ ! -f "$SOURCE_FENS" ]]; then
  echo "missing source FENs: $SOURCE_FENS" >&2
  exit 1
fi

mkdir -p "$OUT"

python3 tools/policy/label_uci.py \
  --engine "$ENGINE" \
  --teacher-profile "$TEACHER_PROFILE" \
  --nodes "$NODES" \
  --threads "$THREADS" \
  --hash "$HASH" \
  --input "$SOURCE_FENS" \
  --output "$OUT/labels.jsonl" \
  --limit "$LIMIT" \
  --resume \
  --flush-every 1000 \
  --option UsePolicy=false

python3 tools/policy/pack_policy_dataset.py \
  --input "$OUT/labels.jsonl" \
  --output "$OUT/dataset.npz" \
  --quiet-only

OMP_NUM_THREADS="$TORCH_THREADS" \
MKL_NUM_THREADS="$TORCH_THREADS" \
VECLIB_MAXIMUM_THREADS="$TORCH_THREADS" \
TORCH_NUM_THREADS="$TORCH_THREADS" \
python3 tools/policy/train_smoke_policy.py \
  --data "$OUT/dataset.npz" \
  --out-dir "$OUT/train" \
  --epochs "$EPOCHS" \
  --batch-groups "$BATCH_GROUPS" \
  --hidden "$HIDDEN" \
  --device "$DEVICE" \
  --torch-threads "$TORCH_THREADS"

python3 tools/policy/export_policy.py \
  --checkpoint "$OUT/train/smoke-policy.pt" \
  --output "$OUT/train/smoke-policy.bin"

echo "rinnegan policy metrics: $OUT/train/metrics.json"
echo "rinnegan policy binary : $OUT/train/smoke-policy.bin"
