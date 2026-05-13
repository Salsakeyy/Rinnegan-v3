#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

SOURCE="${SOURCE:-data/policy/stockfish-labels/labels.jsonl}"
OUT="${OUT:-data/policy/stockfish-200k}"
LIMIT="${LIMIT:-200000}"
EPOCHS="${EPOCHS:-3}"
BATCH_GROUPS="${BATCH_GROUPS:-512}"
HIDDEN="${HIDDEN:-256}"

if [[ ! -f "$SOURCE" ]]; then
  echo "missing policy source labels: $SOURCE" >&2
  exit 1
fi

mkdir -p "$OUT"

python3 tools/policy/pack_policy_dataset.py \
  --input "$SOURCE" \
  --output "$OUT/dataset.npz" \
  --limit "$LIMIT"

python3 tools/policy/train_smoke_policy.py \
  --data "$OUT/dataset.npz" \
  --out-dir "$OUT/train" \
  --epochs "$EPOCHS" \
  --batch-groups "$BATCH_GROUPS" \
  --hidden "$HIDDEN"

python3 tools/policy/export_policy.py \
  --checkpoint "$OUT/train/smoke-policy.pt" \
  --output "$OUT/train/smoke-policy.bin"

echo "policy sample metrics: $OUT/train/metrics.json"
echo "policy sample binary : $OUT/train/smoke-policy.bin"
