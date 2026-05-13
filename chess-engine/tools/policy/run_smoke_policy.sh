#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

OUT="${OUT:-data/policy/smoke}"
COUNT="${COUNT:-64}"
DEPTH="${DEPTH:-2}"
EPOCHS="${EPOCHS:-4}"
ENGINE="${ENGINE:-build/engine}"
TEACHER_PROFILE="${TEACHER_PROFILE:-rinnegan}"
THREADS="${THREADS:-1}"
HASH="${HASH:-16}"

mkdir -p "$OUT"

if [[ ! -x "$ENGINE" ]]; then
  echo "missing executable engine: $ENGINE" >&2
  echo "build it first with: cmake --build build --target engine" >&2
  exit 1
fi

python3 tools/policy/generate_fens.py \
  --count "$COUNT" \
  --openings tests/openings.epd \
  --output "$OUT/fens.txt" \
  --seed 17 \
  --min-ply 8 \
  --max-ply 60 \
  --ply-buckets "8-16:0.45,17-36:0.40,37-60:0.15" \
  --progress 0

python3 tools/policy/label_uci.py \
  --engine "$ENGINE" \
  --teacher-profile "$TEACHER_PROFILE" \
  --depth "$DEPTH" \
  --threads "$THREADS" \
  --hash "$HASH" \
  --input "$OUT/fens.txt" \
  --output "$OUT/labels.jsonl" \
  --no-resume \
  --flush-every 8

python3 tools/policy/pack_policy_dataset.py \
  --input "$OUT/labels.jsonl" \
  --output "$OUT/dataset.npz"

python3 tools/policy/train_smoke_policy.py \
  --data "$OUT/dataset.npz" \
  --out-dir "$OUT/train" \
  --epochs "$EPOCHS" \
  --batch-groups 16 \
  --hidden 64

echo "policy smoke metrics: $OUT/train/metrics.json"
