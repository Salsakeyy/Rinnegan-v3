#!/usr/bin/env bash
#
# Runs a SPRT match between Rinnegan v2 (engine_v2) and Rinnegan v1 (engine_v1).
#
# Null hypothesis   H0: elo(v2 - v1) = 0   ("v2 is not stronger")
# Alt. hypothesis   H1: elo(v2 - v1) = 10  ("v2 is >= 10 ELO stronger")
# Stops automatically when LLR crosses +2.94 (H1 accepted) or -2.94 (H0 accepted).
#
# Usage:   bash tests/sprt_v2_vs_v1.sh [total_games]
# Default: SPRT with a soft cap of 5000 rounds (10000 games).
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

V1="$ROOT/build/engine_v1"
V2="$ROOT/build/engine_v2"
BOOK="$HERE/openings.epd"
PGN="$HERE/v2_vs_v1.pgn"

for bin in "$V1" "$V2"; do
    if [[ ! -x "$bin" ]]; then
        echo "ERROR: missing binary $bin" >&2
        echo "Build first:  cd $ROOT && cmake --build build" >&2
        echo "Then save copies:  cp build/engine build/engine_v2  (and engine_v1 from main)" >&2
        exit 1
    fi
done

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
  - cutechess-cli  (Debian/Ubuntu:  sudo apt install cutechess-cli
                    or build from https://github.com/cutechess/cutechess)
  - fastchess      (https://github.com/Disservin/fastchess)

Then rerun this script.
EOF
    exit 2
fi

ROUNDS="${1:-5000}"
CONCURRENCY="$(nproc 2>/dev/null || echo 2)"
TC="10+0.1"

echo "Runner       : $RUNNER"
echo "Engines      : v2=$V2"
echo "               v1=$V1"
echo "Time control : $TC"
echo "Rounds (max) : $ROUNDS  (games = 2 * rounds)"
echo "Concurrency  : $CONCURRENCY"
echo "Book         : $BOOK"
echo "PGN output   : $PGN"
echo

if [[ "$RUNNER" == "cutechess-cli" ]]; then
    exec cutechess-cli \
        -engine cmd="$V2" name=Rinnegan_v2 \
        -engine cmd="$V1" name=Rinnegan_v1 \
        -each proto=uci tc="$TC" option.Hash=16 \
        -rounds "$ROUNDS" -games 2 -repeat -concurrency "$CONCURRENCY" \
        -openings file="$BOOK" format=epd order=random \
        -sprt elo0=0 elo1=10 alpha=0.05 beta=0.05 \
        -draw movenumber=40 movecount=8 score=8 \
        -resign movecount=4 score=800 \
        -pgnout "$PGN"
else
    # fastchess: similar flags, slightly different syntax
    exec "$RUNNER" \
        -engine cmd="$V2" name=Rinnegan_v2 \
        -engine cmd="$V1" name=Rinnegan_v1 \
        -each proto=uci tc="$TC" option.Hash=16 \
        -rounds "$ROUNDS" -games 2 -repeat -concurrency "$CONCURRENCY" \
        -openings file="$BOOK" format=epd order=random \
        -sprt elo0=0 elo1=10 alpha=0.05 beta=0.05 \
        -draw movenumber=40 movecount=8 score=8 \
        -resign movecount=4 score=800 \
        -pgnout "$PGN"
fi
