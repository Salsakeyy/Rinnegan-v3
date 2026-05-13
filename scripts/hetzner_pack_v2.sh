#!/usr/bin/env bash
# Pack a v2 dataset on Hetzner. Reads JSONL labels (from hetzner_label_v2.sh)
# and writes a v2 .npz consumed by tools/policy/v2/train_v2.py.
#
# Env:
#   LABELS         path to labels.jsonl
#   OUT            output .npz path
#   FEATURE_SPACE  split | new32_compat (default split for R&D)
#   PROPOSAL_MODE  all_legal | proposal_set
#   TOP_QUIET      number of top-baseline quiets in proposal mode
#   RAND_QUIET     number of random quiets in proposal mode
#   LIMIT          0 = no cap

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/chess-engine"

LABELS="${LABELS:-data/policy/v2/labels-stockfish-multipv/labels.jsonl}"
OUT="${OUT:-data/policy/v2/dataset.npz}"
FEATURE_SPACE="${FEATURE_SPACE:-split}"
PROPOSAL_MODE="${PROPOSAL_MODE:-all_legal}"
TOP_QUIET="${TOP_QUIET:-8}"
RAND_QUIET="${RAND_QUIET:-4}"
LIMIT="${LIMIT:-0}"

mkdir -p "$(dirname "$OUT")"

python3 tools/policy/v2/pack_v2.py \
  --input "$LABELS" \
  --output "$OUT" \
  --feature-space "$FEATURE_SPACE" \
  --proposal-mode "$PROPOSAL_MODE" \
  --proposal-top-quiet "$TOP_QUIET" \
  --proposal-random-quiet "$RAND_QUIET" \
  --limit "$LIMIT"
