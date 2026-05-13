#!/usr/bin/env bash
# Stockfish-adjudicate changed moves from the v2 root-set bench.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/chess-engine"

IN_DIR="${IN_DIR:-data/policy/v2/rootset/400k_qr_s400_eg0}"
INPUT="${INPUT:-$IN_DIR/rootset.jsonl}"
OUT_DIR="${OUT_DIR:-$IN_DIR/sf_adjudication}"
STOCKFISH="${STOCKFISH:-$(command -v stockfish || true)}"
if [[ -z "$STOCKFISH" && -x /opt/homebrew/bin/stockfish ]]; then
  STOCKFISH=/opt/homebrew/bin/stockfish
fi
if [[ -z "$STOCKFISH" && -x /usr/local/bin/stockfish ]]; then
  STOCKFISH=/usr/local/bin/stockfish
fi
if [[ -z "$STOCKFISH" && -x /usr/games/stockfish ]]; then
  STOCKFISH=/usr/games/stockfish
fi

DEPTH="${DEPTH:-10}"
THREADS="${THREADS:-1}"
HASH_MB="${HASH_MB:-64}"
MAX_CHANGED_PER_CONFIG="${MAX_CHANGED_PER_CONFIG:-200}"
CONFIG_LABEL="${CONFIG_LABEL:-}"

if [[ -z "$STOCKFISH" ]]; then
  echo "ERROR: stockfish not found; set STOCKFISH=/path/to/stockfish" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

args=(
  python3 tools/policy/v2/rootset_sf_adjudicate.py
  --input "$INPUT"
  --stockfish "$STOCKFISH"
  --max-changed-per-config "$MAX_CHANGED_PER_CONFIG"
  --depth "$DEPTH"
  --threads "$THREADS"
  --hash-mb "$HASH_MB"
  --out-jsonl "$OUT_DIR/adjudication.jsonl"
  --out-csv "$OUT_DIR/adjudication.csv"
  --summary-md "$OUT_DIR/adjudication_summary.md"
)

if [[ -n "$CONFIG_LABEL" ]]; then
  args+=(--config-label "$CONFIG_LABEL")
fi

printf '%q ' "${args[@]}" > "$OUT_DIR/cmd.txt"
printf '\n' >> "$OUT_DIR/cmd.txt"

"${args[@]}"
