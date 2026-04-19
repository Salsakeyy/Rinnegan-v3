#!/usr/bin/env bash
#
# Runs a SPRT match between Rinnegan v4 (engine_v4) and Rinnegan v3 (engine_v3).
#
# Null hypothesis   H0: elo(v4 - v3) = 0
# Alt. hypothesis   H1: elo(v4 - v3) = 15
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

V3="${V3:-$ROOT/build/engine_v3}"
V4="${V4:-$ROOT/build/engine_v4}"
NET="${NET:-$ROOT/rinnegan-v4.net}"
BOOK="$HERE/openings.epd"
PGN="$HERE/v4_vs_v3.pgn"

for bin in "$V3" "$V4"; do
    if [[ ! -x "$bin" ]]; then
        echo "ERROR: missing binary $bin" >&2
        echo "Build first:  cd $ROOT && cmake --build build" >&2
        echo "Then save copies:  cp build/engine build/engine_v4" >&2
        echo "                    (and build/engine_v3 from the v3 commit)" >&2
        exit 1
    fi
done

if [[ ! -f "$NET" ]]; then
    echo "ERROR: NNUE net not found at $NET" >&2
    exit 1
fi

if [[ ! -f "$BOOK" ]]; then
    echo "ERROR: opening book not found at $BOOK" >&2
    exit 1
fi

RUNNER=""
if command -v cutechess-cli >/dev/null 2>&1; then
    RUNNER="cutechess-cli"
elif command -v fastchess >/dev/null 2>&1; then
    RUNNER="fastchess"
elif command -v fast-chess >/dev/null 2>&1; then
    RUNNER="fast-chess"
fi

if [[ -z "$RUNNER" ]]; then
    cat >&2 <<'EOF'
ERROR: no SPRT runner found on PATH.

Install one of:
  - cutechess-cli
  - fastchess
EOF
    exit 2
fi

ROUNDS="${1:-5000}"
CONCURRENCY="${CONCURRENCY:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}"
TC="${TC:-10+0.1}"

echo "Runner       : $RUNNER"
echo "Engines      : v4=$V4"
echo "               v3=$V3"
echo "NNUE net     : $NET"
echo "Time control : $TC"
echo "Rounds (max) : $ROUNDS  (games = 2 * rounds)"
echo "Concurrency  : $CONCURRENCY"
echo "Book         : $BOOK"
echo "PGN output   : $PGN"
echo

if [[ "$RUNNER" == "cutechess-cli" ]]; then
    exec cutechess-cli \
        -engine cmd="$V4" name=Rinnegan_v4 option.EvalFile="$NET" option.UseNNUE=true \
        -engine cmd="$V3" name=Rinnegan_v3 \
        -each proto=uci tc="$TC" option.Hash=16 option.Threads=1 \
        -rounds "$ROUNDS" -games 2 -repeat -concurrency "$CONCURRENCY" \
        -openings file="$BOOK" format=epd order=random \
        -sprt elo0=0 elo1=15 alpha=0.05 beta=0.05 \
        -draw movenumber=40 movecount=8 score=8 \
        -resign movecount=4 score=800 \
        -pgnout "$PGN"
else
    exec "$RUNNER" \
        -engine cmd="$V4" name=Rinnegan_v4 option.EvalFile="$NET" option.UseNNUE=true \
        -engine cmd="$V3" name=Rinnegan_v3 \
        -each proto=uci tc="$TC" option.Hash=16 option.Threads=1 \
        -rounds "$ROUNDS" -games 2 -repeat -concurrency "$CONCURRENCY" \
        -openings file="$BOOK" format=epd order=random \
        -sprt elo0=0 elo1=15 alpha=0.05 beta=0.05 \
        -draw movenumber=40 movecount=8 score=8 \
        -resign movecount=4 score=800 \
        -pgnout file="$PGN"
fi
