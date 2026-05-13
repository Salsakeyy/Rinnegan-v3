#!/usr/bin/env bash
# Run fused offline metrics on a v2 dataset, optionally with a trained
# checkpoint. See tools/policy/v2/eval_v2.py for what's reported.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/chess-engine"

DATA="${DATA:-data/policy/v2/dataset.npz}"
MODEL="${MODEL:-}"
LAMBDA="${LAMBDA:-1.0}"
TOPK="${TOPK:-5}"
OUT="${OUT:-}"

ARGS=(--data "$DATA" --lambda "$LAMBDA" --top-k "$TOPK")
if [[ -n "$MODEL" ]]; then
  ARGS+=(--model "$MODEL")
fi
if [[ -n "$OUT" ]]; then
  ARGS+=(--output "$OUT")
fi

python3 tools/policy/v2/eval_v2.py "${ARGS[@]}"
