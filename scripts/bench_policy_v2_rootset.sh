#!/usr/bin/env bash
# Larger pre-SPRT root-set bench for policy v2 configs.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/chess-engine"

ENGINE="${ENGINE:-build/engine}"
FENS="${FENS:-data/policy/policy-10m-fast/dataset.fens.txt}"
OUT_DIR="${OUT_DIR:-data/policy/v2/rootset/400k_qr_s400_eg0}"
SAMPLE_SIZE="${SAMPLE_SIZE:-500}"
DEPTH="${DEPTH:-8}"
THREADS="${THREADS:-1}"
HASH_MB="${HASH_MB:-16}"
SEED="${SEED:-20260511}"
MAX_INPUT_LINES="${MAX_INPUT_LINES:-0}"
TOP_K="${TOP_K:-8}"
CONFIGS="${CONFIGS:-400k_qr_s400_eg0:data/policy/v2/run-stockfish-mpv8-d14d15-400k-h256/policy_eg10.bin:2:400:400:0}"

mkdir -p "$OUT_DIR"

args=(
  python3 tools/policy/v2/rootset_bench.py
  --engine "$ENGINE"
  --fens "$FENS"
  --sample-size "$SAMPLE_SIZE"
  --seed "$SEED"
  --max-input-lines "$MAX_INPUT_LINES"
  --depth "$DEPTH"
  --threads "$THREADS"
  --hash-mb "$HASH_MB"
  --top-k "$TOP_K"
  --out-jsonl "$OUT_DIR/rootset.jsonl"
  --out-csv "$OUT_DIR/rootset.csv"
  --summary-md "$OUT_DIR/rootset_summary.md"
)

IFS=',' read -ra cfgs <<<"$CONFIGS"
for cfg in "${cfgs[@]}"; do
  args+=(--config "$cfg")
done

printf '%q ' "${args[@]}" > "$OUT_DIR/cmd.txt"
printf '\n' >> "$OUT_DIR/cmd.txt"

"${args[@]}"
