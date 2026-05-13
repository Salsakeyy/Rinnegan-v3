#!/usr/bin/env bash
# Train a v2 policy model on Runpod. Expects a v2 dataset .npz already
# uploaded (see scripts/hetzner_pack_v2.sh). Writes a torch checkpoint
# under OUT_DIR; export with tools/policy/v2/export_v2.py afterwards.
#
# Env:
#   DATA            v2 dataset path (.npz or directory)
#   OUT_DIR         where to put best.pt / final.pt
#   ARCH            concat (default, engine-loadable) | trunk
#   LOSS            ce | kl | kl_listwise
#   SOFT_TEMP       softmax temperature on teacher cp (default 80)
#   FUSE_BASELINE   1 to enable, 0 to disable (default 1)
#   BATCH_GROUPS    1024
#   EPOCHS          6
#   LR              3e-4
#   HIDDEN1, HIDDEN2 (concat) / TRUNK_HIDDEN, HEAD_HIDDEN (trunk)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/chess-engine"

DATA="${DATA:-data/policy/v2/dataset.npz}"
OUT_DIR="${OUT_DIR:-data/policy/v2/run-default}"
ARCH="${ARCH:-concat}"
LOSS="${LOSS:-kl}"
SOFT_TEMP="${SOFT_TEMP:-80}"
FUSE_BASELINE="${FUSE_BASELINE:-1}"
BATCH_GROUPS="${BATCH_GROUPS:-1024}"
EPOCHS="${EPOCHS:-6}"
LR="${LR:-3e-4}"
HIDDEN1="${HIDDEN1:-256}"
HIDDEN2="${HIDDEN2:-256}"
TRUNK_HIDDEN="${TRUNK_HIDDEN:-64}"
HEAD_HIDDEN="${HEAD_HIDDEN:-128}"

mkdir -p "$OUT_DIR"

EXTRA_ARGS=()
if [[ "$FUSE_BASELINE" = "1" ]]; then
  EXTRA_ARGS+=(--fuse-baseline)
else
  EXTRA_ARGS+=(--no-fuse-baseline)
fi

if [[ "$ARCH" = "concat" ]]; then
  EXTRA_ARGS+=(--hidden1 "$HIDDEN1" --hidden2 "$HIDDEN2")
else
  EXTRA_ARGS+=(--trunk-hidden "$TRUNK_HIDDEN" --head-hidden "$HEAD_HIDDEN")
fi

python3 tools/policy/v2/train_v2.py \
  --data "$DATA" \
  --out-dir "$OUT_DIR" \
  --epochs "$EPOCHS" \
  --batch-groups "$BATCH_GROUPS" \
  --lr "$LR" \
  --arch "$ARCH" \
  --loss "$LOSS" \
  --soft-temp "$SOFT_TEMP" \
  "${EXTRA_ARGS[@]}"

echo "checkpoint at $OUT_DIR/best.pt and $OUT_DIR/final.pt"
echo "now run: python3 tools/policy/v2/export_v2.py --checkpoint $OUT_DIR/best.pt --output $OUT_DIR/policy.bin"
