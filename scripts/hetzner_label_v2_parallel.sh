#!/usr/bin/env bash
# Parallel companion to hetzner_label_v2.sh.
#
# Splits the input FEN file into N chunks, runs N copies of
# tools/policy/v2/label_multipv.py in parallel (each pinned to 1 thread),
# then concatenates the JSONL outputs. This is the pattern to use on a
# Hetzner box with many cores — one Stockfish per logical CPU.
#
# Env (defaults shown):
#   ENGINE          /opt/homebrew/bin/stockfish on darwin, /usr/local/bin/stockfish on linux
#   FENS            data/policy/v2/source.fens
#   OUT             data/policy/v2/labels-stockfish-multipv
#   TEACHER_MODE    stockfish_multipv
#   MULTIPV         8
#   DEPTH           14
#   PARALLEL        nproc / 2 (leave headroom)
#   HASH_MB         512
#   LIMIT           0 = no cap
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/chess-engine"

if command -v stockfish >/dev/null 2>&1; then
    DEFAULT_ENGINE="$(command -v stockfish)"
else
    DEFAULT_ENGINE=""
fi

ENGINE="${ENGINE:-$DEFAULT_ENGINE}"
FENS="${FENS:-data/policy/v2/source.fens}"
OUT="${OUT:-data/policy/v2/labels-stockfish-multipv}"
TEACHER_MODE="${TEACHER_MODE:-stockfish_multipv}"
MULTIPV="${MULTIPV:-8}"
DEPTH="${DEPTH:-14}"
HASH_MB="${HASH_MB:-512}"
LIMIT="${LIMIT:-0}"

NPROC="$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
PARALLEL="${PARALLEL:-$((NPROC / 2))}"
[[ "$PARALLEL" -lt 1 ]] && PARALLEL=1

if [[ -z "$ENGINE" || ! -x "$ENGINE" ]]; then
    echo "ERROR: teacher binary not found. Set ENGINE=/path/to/stockfish" >&2
    exit 1
fi
if [[ ! -f "$FENS" ]]; then
    echo "ERROR: source FENs not found at $FENS" >&2
    exit 1
fi

mkdir -p "$OUT"
WORK="$OUT/_chunks"
mkdir -p "$WORK"
rm -f "$WORK"/chunk_* "$WORK"/labels_*.jsonl

# Optional cap.
if [[ "$LIMIT" -gt 0 ]]; then
    head -n "$LIMIT" "$FENS" > "$WORK/source.fens"
else
    cp "$FENS" "$WORK/source.fens"
fi
TOTAL=$(wc -l < "$WORK/source.fens")

# Split into PARALLEL roughly-equal chunks. macOS `split` doesn't have
# `-n l/N`, so compute a chunk size from line count instead.
CHUNK_LINES=$(( (TOTAL + PARALLEL - 1) / PARALLEL ))
split -a 3 -d -l "$CHUNK_LINES" "$WORK/source.fens" "$WORK/chunk_"

echo "Engine     : $ENGINE"
echo "Teacher    : $TEACHER_MODE  (depth=$DEPTH, multipv=$MULTIPV)"
echo "Source     : $FENS  ($TOTAL FENs)"
echo "Parallel   : $PARALLEL"
echo "Out dir    : $OUT"
echo

START=$(date +%s)
PIDS=()
for chunk in "$WORK"/chunk_*; do
    name=$(basename "$chunk")
    out="$WORK/labels_${name#chunk_}.jsonl"
    python3 tools/policy/v2/label_multipv.py \
        --engine "$ENGINE" \
        --input  "$chunk" \
        --output "$out" \
        --teacher-mode "$TEACHER_MODE" \
        --multipv "$MULTIPV" \
        --depth   "$DEPTH" \
        --threads 1 \
        --hash    "$HASH_MB" \
        --no-resume > "$WORK/${name}.log" 2>&1 &
    PIDS+=("$!")
done

# Progress: every 10s print how many records each chunk has produced.
trap 'kill "${PIDS[@]}" 2>/dev/null || true' INT TERM
while :; do
    sleep 10
    alive=0
    for pid in "${PIDS[@]}"; do
        kill -0 "$pid" 2>/dev/null && ((alive++)) || true
    done
    written=0
    for f in "$WORK"/labels_*.jsonl; do
        [[ -f "$f" ]] || continue
        n=$(wc -l < "$f")
        written=$((written + n))
    done
    elapsed=$(( $(date +%s) - START ))
    rate="?"
    [[ "$elapsed" -gt 0 ]] && rate=$(awk -v w="$written" -v e="$elapsed" 'BEGIN{printf "%.2f", w/e}')
    echo "[${elapsed}s] alive=$alive written=$written/$TOTAL  rate=${rate}/s"
    [[ "$alive" -eq 0 ]] && break
done

# Wait so we surface non-zero exit codes from any worker.
fail=0
for pid in "${PIDS[@]}"; do
    wait "$pid" || fail=$((fail + 1))
done

# Merge.
cat "$WORK"/labels_*.jsonl > "$OUT/labels.jsonl"
RECORDS=$(wc -l < "$OUT/labels.jsonl")
ELAPSED=$(( $(date +%s) - START ))
echo
echo "wrote $RECORDS records in ${ELAPSED}s ($(awk -v r="$RECORDS" -v e="$ELAPSED" 'BEGIN{printf "%.2f", r/e}') FENs/s) → $OUT/labels.jsonl"
[[ "$fail" -gt 0 ]] && { echo "WARN: $fail chunk(s) returned non-zero" >&2; }

# Save command for reproducibility.
cat > "$OUT/cmd.txt" <<EOF
ENGINE=$ENGINE
FENS=$FENS
OUT=$OUT
TEACHER_MODE=$TEACHER_MODE
MULTIPV=$MULTIPV
DEPTH=$DEPTH
PARALLEL=$PARALLEL
HASH_MB=$HASH_MB
LIMIT=$LIMIT
TOTAL=$TOTAL
ELAPSED_S=$ELAPSED
RECORDS=$RECORDS
EOF
