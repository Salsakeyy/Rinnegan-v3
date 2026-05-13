#!/usr/bin/env bash
# Run on Hetzner. Generates teacher labels for the v2 pipeline.
#
# Three teacher modes (see docs/policy_v2_plan.md):
#   stockfish_multipv      Strong external teacher; default and preferred.
#   rinnegan_deep_multipv  Self-teacher with deeper search and MultiPV.
#   hard_bestmove_fallback Single bestmove per FEN (compat shape).
#
# Required env (defaults shown):
#   ENGINE          path to teacher binary (Stockfish or Rinnegan)
#   FENS            path to a list of FENs / EPDs (one per line, or JSONL with `fen`)
#   OUT             output directory
#   TEACHER_MODE    stockfish_multipv (default)
#   MULTIPV         8
#   DEPTH           18
#   THREADS         host CPU count
#   HASH_MB         1024

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/chess-engine"

ENGINE="${ENGINE:-/usr/local/bin/stockfish}"
FENS="${FENS:-data/policy/source-fens.txt}"
OUT="${OUT:-data/policy/v2/labels-stockfish-multipv}"
TEACHER_MODE="${TEACHER_MODE:-stockfish_multipv}"
MULTIPV="${MULTIPV:-8}"
DEPTH="${DEPTH:-18}"
NODES="${NODES:-0}"
MOVETIME="${MOVETIME:-0}"
THREADS="${THREADS:-$(nproc 2>/dev/null || echo 4)}"
HASH_MB="${HASH_MB:-1024}"
LIMIT="${LIMIT:-0}"

mkdir -p "$OUT"

python3 tools/policy/v2/label_multipv.py \
  --engine "$ENGINE" \
  --input  "$FENS" \
  --output "$OUT/labels.jsonl" \
  --teacher-mode "$TEACHER_MODE" \
  --multipv "$MULTIPV" \
  --depth   "$DEPTH" \
  --nodes   "$NODES" \
  --movetime "$MOVETIME" \
  --threads "$THREADS" \
  --hash    "$HASH_MB" \
  --limit   "$LIMIT" \
  --resume

echo "labels at $OUT/labels.jsonl"
