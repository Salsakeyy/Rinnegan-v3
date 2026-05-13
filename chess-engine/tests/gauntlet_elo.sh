#!/usr/bin/env bash
#
# Estimate engine Elo by playing a multi-anchor fastchess gauntlet.
#
# Serious CCRL-style estimate:
#   bash tests/gauntlet_elo.sh ./rinnegan-pgo --ccrl --rounds 1000 --tc 10+0.1
#
# Quick smoke estimate with the Stockfish UCI_Elo anchors:
#   bash tests/gauntlet_elo.sh ./rinnegan-pgo --rounds 100 --tc 5+0.05
#
# Backward-compatible form:
#   bash tests/gauntlet_elo.sh ENGINE [ROUNDS] [TC]

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

ENGINE=""
ROUNDS="1000"
TC="10+0.1"
ANCHORS_CONF="$HERE/anchors.conf"
RESULTS_BASE="$HERE/gauntlet_results"
TARGET_ERR="20"
HASH="64"
THREADS="1"
CONCURRENCY="${CONCURRENCY:-}"
DRY_RUN=0

usage() {
    sed -n '1,12p' "$0"
    cat <<EOF

Options:
  --ccrl                 use tests/ccrl_anchors.conf
  --anchors FILE         anchor config file
  --rounds N             rounds per anchor; games per anchor = 2 * N
  --tc TC                fastchess time control, e.g. 10+0.1
  --target-error ELO     report target, default 20
  --hash MB              Hash option sent through -each, default 64
  --threads N            Threads option for the tested engine, default 1
  --concurrency N        simultaneous games, default min(cpu, 4)
  --out-dir DIR          base output directory
  --dry-run              validate anchor commands without playing
  -h, --help             show this help

Anchor config format:
  NAME | CCRL_ELO | COMMAND | FASTCHESS_ENGINE_OPTIONS
EOF
}

positional=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --ccrl) ANCHORS_CONF="$HERE/ccrl_anchors.conf"; shift ;;
        --anchors) ANCHORS_CONF="$2"; shift 2 ;;
        --rounds) ROUNDS="$2"; shift 2 ;;
        --tc) TC="$2"; shift 2 ;;
        --target-error) TARGET_ERR="$2"; shift 2 ;;
        --hash) HASH="$2"; shift 2 ;;
        --threads) THREADS="$2"; shift 2 ;;
        --concurrency) CONCURRENCY="$2"; shift 2 ;;
        --out-dir) RESULTS_BASE="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --*) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
        *) positional+=("$1"); shift ;;
    esac
done

if [[ ${#positional[@]} -gt 0 ]]; then ENGINE="${positional[0]}"; fi
if [[ ${#positional[@]} -gt 1 ]]; then ROUNDS="${positional[1]}"; fi
if [[ ${#positional[@]} -gt 2 ]]; then TC="${positional[2]}"; fi
if [[ ${#positional[@]} -gt 3 ]]; then
    echo "ERROR: too many positional arguments" >&2
    usage >&2
    exit 2
fi

ENGINE="${ENGINE:-$ROOT/rinnegan-pgo}"

trim() {
    local value="$1"
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    printf '%s' "$value"
}

safe_name() {
    printf '%s' "$1" | tr -cs 'A-Za-z0-9_.-' '_'
}

cpu_count() {
    nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2
}

if [[ -z "$CONCURRENCY" ]]; then
    cores="$(cpu_count)"
    if [[ "$cores" -gt 4 ]]; then CONCURRENCY=4; else CONCURRENCY="$cores"; fi
fi

resolve_cmd() {
    local cmd="$1"
    local resolved=""

    if [[ "$cmd" == /* && -x "$cmd" ]]; then
        printf '%s' "$cmd"
        return 0
    fi
    if [[ -x "$ROOT/$cmd" ]]; then
        printf '%s' "$ROOT/$cmd"
        return 0
    fi
    if [[ -x "$HERE/$cmd" ]]; then
        printf '%s' "$HERE/$cmd"
        return 0
    fi
    resolved="$(command -v "$cmd" 2>/dev/null || true)"
    if [[ -n "$resolved" && -x "$resolved" ]]; then
        printf '%s' "$resolved"
        return 0
    fi
    return 1
}

[[ -x "$ENGINE" ]] || { echo "ERROR: engine not found or not executable: $ENGINE" >&2; exit 1; }
[[ -f "$ANCHORS_CONF" ]] || { echo "ERROR: anchors config not found: $ANCHORS_CONF" >&2; exit 1; }
[[ -f "$HERE/openings.epd" ]] || { echo "ERROR: opening book not found: $HERE/openings.epd" >&2; exit 1; }
command -v fastchess >/dev/null 2>&1 || {
    echo "ERROR: fastchess not on PATH. Install from https://github.com/Disservin/fastchess/releases" >&2
    exit 2
}

declare -a NAMES ELOS CMDS OPTS
while IFS= read -r line; do
    line="${line%%#*}"
    line="$(trim "$line")"
    [[ -z "$line" ]] && continue

    IFS='|' read -r name elo cmd opts <<< "$line"
    name="$(trim "${name:-}")"
    elo="$(trim "${elo:-}")"
    cmd="$(trim "${cmd:-}")"
    opts="$(trim "${opts:-}")"
    [[ -n "$name" && -n "$elo" && -n "$cmd" ]] || continue

    NAMES+=("$name")
    ELOS+=("$elo")
    CMDS+=("$cmd")
    OPTS+=("$opts")
done < "$ANCHORS_CONF"

if [[ ${#NAMES[@]} -eq 0 ]]; then
    echo "ERROR: no anchors found in $ANCHORS_CONF" >&2
    exit 3
fi

ENGINE_NAME="$(safe_name "$(basename "$ENGINE")")"
STAMP="$(date +%Y%m%d-%H%M%S)"
RESULTS_DIR="$RESULTS_BASE/${ENGINE_NAME}_${STAMP}"
CSV="$RESULTS_DIR/results.csv"
REPORT="$RESULTS_DIR/report.md"

if [[ "$DRY_RUN" -eq 0 ]]; then
    mkdir -p "$RESULTS_DIR"
    echo "anchor,anchor_elo,diff,err,estimate,games,pgn,log" > "$CSV"
fi

echo "==================================================================="
echo "Gauntlet Elo estimation"
echo "  Engine       : $ENGINE"
echo "  Engine name  : $ENGINE_NAME"
echo "  Anchors      : $ANCHORS_CONF"
echo "  Rounds       : $ROUNDS per anchor ($((2 * ROUNDS)) games/anchor)"
echo "  Time control : $TC"
echo "  Hash         : $HASH MB"
echo "  Threads      : $THREADS for tested engine"
echo "  Concurrency  : $CONCURRENCY"
echo "  Target error : +/- $TARGET_ERR Elo"
if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "  Output       : dry run, no files written"
else
    echo "  Output       : $RESULTS_DIR"
fi
echo "==================================================================="

if [[ "$DRY_RUN" -eq 1 ]]; then
    echo
    echo "Anchor validation:"
fi

declare -a RESULT_NAMES RESULT_ANCHOR_ELO RESULT_DIFFS RESULT_ERRS RESULT_GAMES
for i in "${!NAMES[@]}"; do
    name="${NAMES[$i]}"
    anchor_elo="${ELOS[$i]}"
    cmd="${CMDS[$i]}"
    opts="${OPTS[$i]}"

    if ! resolved="$(resolve_cmd "$cmd")"; then
        echo "SKIP $name: command not found/executable: $cmd"
        continue
    fi

    if [[ "$DRY_RUN" -eq 1 ]]; then
        echo "  OK $name ($anchor_elo): $resolved $opts"
        continue
    fi

    safe_anchor="$(safe_name "$name")"
    pgn="$RESULTS_DIR/${ENGINE_NAME}_vs_${safe_anchor}.pgn"
    log="$RESULTS_DIR/${ENGINE_NAME}_vs_${safe_anchor}.log"

    echo
    echo "-------------------------------------------------------------------"
    echo ">> $ENGINE_NAME vs $name (CCRL anchor $anchor_elo)"
    echo "-------------------------------------------------------------------"

    rm -f "$pgn" "$log"
    set +e
    # shellcheck disable=SC2086
    fastchess \
        -engine cmd="$ENGINE" name="$ENGINE_NAME" option.Threads="$THREADS" \
        -engine cmd="$resolved" name="$name" $opts \
        -each proto=uci tc="$TC" option.Hash="$HASH" \
        -rounds "$ROUNDS" -games 2 -repeat -concurrency "$CONCURRENCY" \
        -openings file="$HERE/openings.epd" format=epd order=random \
        -draw movenumber=40 movecount=8 score=8 \
        -resign movecount=4 score=800 \
        -pgnout file="$pgn" 2>&1 | tee "$log" | tail -16
    rc=${PIPESTATUS[0]}
    set -e

    if [[ "$rc" -ne 0 ]]; then
        echo "WARN: fastchess exited with status $rc for $name; attempting to parse partial result"
    fi

    elo_line="$(grep -E '^Elo: ' "$log" | tail -1 || true)"
    if [[ -z "$elo_line" ]]; then
        echo "WARN: no Elo line for $name; skipping this anchor in combined estimate"
        continue
    fi

    read -r diff err < <(awk '{
        e = $4; gsub(",", "", e);
        print $2, e
    }' <<< "$elo_line")

    games=0
    if [[ -f "$pgn" ]]; then
        games="$(grep -c '^\[Event ' "$pgn" || true)"
    fi
    if [[ "$games" -eq 0 ]]; then
        games=$((2 * ROUNDS))
    fi

    estimate="$(awk -v a="$anchor_elo" -v d="$diff" 'BEGIN{printf "%.1f", a + d}')"
    printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$name" "$anchor_elo" "$diff" "$err" "$estimate" "$games" "$pgn" "$log" >> "$CSV"

    RESULT_NAMES+=("$name")
    RESULT_ANCHOR_ELO+=("$anchor_elo")
    RESULT_DIFFS+=("$diff")
    RESULT_ERRS+=("$err")
    RESULT_GAMES+=("$games")
done

if [[ "$DRY_RUN" -eq 1 ]]; then
    exit 0
fi

if [[ ${#RESULT_NAMES[@]} -eq 0 ]]; then
    echo "ERROR: no usable anchor results available" >&2
    exit 4
fi

combined="$(awk -F, '
    function isnum(s) { return s ~ /^[-+]?[0-9]+(\.[0-9]+)?$/ }
    NR == 1 { next }
    {
        if (!isnum($3) || !isnum($4) || $4 <= 0) next
        w = 1.0 / ($4 * $4)
        sumw += w
        sumwx += w * $5
        kept += 1
        games += $6
    }
    END {
        if (kept == 0 || sumw == 0) {
            printf "nan nan 0 0\n"; exit
        }
        elo = sumwx / sumw
        err = 1.0 / sqrt(sumw)
        printf "%.1f %.1f %d %d\n", elo, err, kept, games
    }
' "$CSV")"

combined_elo="$(cut -d' ' -f1 <<< "$combined")"
combined_err="$(cut -d' ' -f2 <<< "$combined")"
combined_kept="$(cut -d' ' -f3 <<< "$combined")"
combined_games="$(cut -d' ' -f4 <<< "$combined")"

needed_rounds="$(awk -v rounds="$ROUNDS" -v err="$combined_err" -v target="$TARGET_ERR" '
    BEGIN {
        if (err == "nan" || err <= target) { print rounds; exit }
        need = rounds * (err / target) * (err / target)
        printf "%d", int(need + 0.999)
    }
')"

{
    echo "# Gauntlet Elo Report"
    echo
    echo "- Engine: \`$ENGINE\`"
    echo "- Anchors: \`$ANCHORS_CONF\`"
    echo "- Time control: \`$TC\`"
    echo "- Rounds per anchor: \`$ROUNDS\`"
    echo "- Target engine threads: \`$THREADS\`"
    echo "- Hash: \`$HASH MB\`"
    echo "- Concurrency: \`$CONCURRENCY\`"
    echo "- Total parsed games: \`$combined_games\`"
    echo
    echo "## Per-Anchor Results"
    echo
    echo "| Anchor | CCRL Elo | Diff | Estimate | Error | Games |"
    echo "|---|---:|---:|---:|---:|---:|"
    awk -F, 'NR > 1 { printf "| %s | %s | %s | %s | %s | %s |\n", $1, $2, $3, $5, $4, $6 }' "$CSV"
    echo
    echo "## Combined Estimate"
    echo
    if [[ "$combined_elo" == "nan" ]]; then
        echo "No finite combined estimate. Run more games or choose closer anchors."
    else
        echo "**$ENGINE_NAME ~= $combined_elo +/- $combined_err Elo**"
        echo
        if awk -v err="$combined_err" -v target="$TARGET_ERR" 'BEGIN{exit !(err > target)}'; then
            echo "Target +/- $TARGET_ERR not reached. Assuming error scales with sqrt(games), rerun around:"
            echo
            echo "\`\`\`bash"
            echo "bash tests/gauntlet_elo.sh \"$ENGINE\" --anchors \"$ANCHORS_CONF\" --rounds $needed_rounds --tc \"$TC\" --target-error $TARGET_ERR --hash $HASH --threads $THREADS --concurrency $CONCURRENCY"
            echo "\`\`\`"
        else
            echo "Target +/- $TARGET_ERR reached."
        fi
    fi
} > "$REPORT"

echo
echo "==================================================================="
echo "Per-anchor Elo estimates"
echo "==================================================================="
printf "%-24s %10s %10s %12s %10s %8s\n" "anchor" "ccrl_elo" "diff" "estimate" "+/-" "games"
awk -F, 'NR > 1 { printf "%-24s %10s %10s %12s %10s %8s\n", $1, $2, $3, $5, $4, $6 }' "$CSV"

echo
echo "==================================================================="
if [[ "$combined_elo" == "nan" ]]; then
    echo "Combined estimate: unavailable"
else
    printf "Combined estimate: %s ~= %s +/- %s Elo (%s anchors, %s games)\n" \
        "$ENGINE_NAME" "$combined_elo" "$combined_err" "$combined_kept" "$combined_games"
    if awk -v err="$combined_err" -v target="$TARGET_ERR" 'BEGIN{exit !(err > target)}'; then
        echo "Target +/- $TARGET_ERR not reached. Approximate rounds/anchor needed: $needed_rounds"
    else
        echo "Target +/- $TARGET_ERR reached."
    fi
fi
echo "CSV    : $CSV"
echo "Report : $REPORT"
echo "==================================================================="
