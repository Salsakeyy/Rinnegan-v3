#!/usr/bin/env bash
#
# Estimate the absolute Elo of a chess engine by playing a gauntlet against
# several anchor engines with known Elo, then combining the results as an
# inverse-variance-weighted average.
#
# Usage:
#     bash tests/gauntlet_elo.sh ENGINE [ROUNDS] [TC]
#
#         ENGINE  path to the engine binary to rate      (default: build/engine_v2)
#         ROUNDS  rounds per anchor; games = 2 * rounds  (default: 150)
#         TC      fastchess time control string          (default: 5+0.05)
#
# Examples:
#     bash tests/gauntlet_elo.sh                         # rate engine_v2 vs all anchors
#     bash tests/gauntlet_elo.sh build/engine_v1 100     # rate v1 with 100 rounds each
#
# Requires: fastchess on PATH, tests/anchors.conf, tests/openings.epd.
# Anchors are defined in tests/anchors.conf (see that file for format).

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

ENGINE="${1:-$ROOT/build/engine_v2}"
ROUNDS="${2:-150}"
TC="${3:-5+0.05}"

ANCHORS_CONF="$HERE/anchors.conf"
BOOK="$HERE/openings.epd"
RESULTS_DIR="$HERE/gauntlet_results"

[[ -x "$ENGINE" ]]     || { echo "ERROR: engine not found or not executable: $ENGINE" >&2; exit 1; }
[[ -f "$ANCHORS_CONF" ]] || { echo "ERROR: anchors config not found: $ANCHORS_CONF" >&2; exit 1; }
[[ -f "$BOOK" ]]       || { echo "ERROR: opening book not found: $BOOK" >&2; exit 1; }
command -v fastchess >/dev/null 2>&1 || {
    echo "ERROR: fastchess not on PATH. Install from" >&2
    echo "  https://github.com/Disservin/fastchess/releases" >&2
    exit 2
}

mkdir -p "$RESULTS_DIR"
ENGINE_NAME="$(basename "$ENGINE")"
CONCURRENCY="$(nproc 2>/dev/null || echo 2)"

echo "==================================================================="
echo "Gauntlet Elo estimation"
echo "  Engine      : $ENGINE  (name=$ENGINE_NAME)"
echo "  Rounds/anchor: $ROUNDS  (games/anchor = $((2 * ROUNDS)))"
echo "  Time control: $TC"
echo "  Concurrency : $CONCURRENCY"
echo "  Anchors conf: $ANCHORS_CONF"
echo "==================================================================="

# Parse anchors into parallel arrays.
declare -a NAMES ELOS CMDS OPTS
while IFS= read -r line; do
    # Strip comments and blank lines
    line="${line%%#*}"
    line="$(echo "$line" | xargs)"
    [[ -z "$line" ]] && continue
    IFS='|' read -r name elo cmd opts <<< "$line"
    NAMES+=("$(echo "$name" | xargs)")
    ELOS+=("$(echo "$elo" | xargs)")
    CMDS+=("$(echo "$cmd" | xargs)")
    OPTS+=("$(echo "$opts" | xargs)")
done < "$ANCHORS_CONF"

# Play one gauntlet pairing per anchor.
declare -a RESULT_ELOS RESULT_ERRS RESULT_NAMES RESULT_ANCHOR_ELO
for i in "${!NAMES[@]}"; do
    name="${NAMES[$i]}"
    elo="${ELOS[$i]}"
    cmd="${CMDS[$i]}"
    opts="${OPTS[$i]}"
    # Resolve bare command names via PATH (e.g. "stockfish" on macOS Homebrew).
    if [[ ! -x "$cmd" ]]; then
        resolved="$(command -v "$cmd" 2>/dev/null || true)"
        if [[ -n "$resolved" && -x "$resolved" ]]; then
            cmd="$resolved"
        else
            echo "skipping $name: $cmd not executable and not on PATH"
            continue
        fi
    fi

    echo
    echo "-------------------------------------------------------------------"
    echo ">> $ENGINE_NAME vs $name (anchor Elo $elo)"
    echo "-------------------------------------------------------------------"

    pgn="$RESULTS_DIR/${ENGINE_NAME}_vs_${name}.pgn"
    log="$RESULTS_DIR/${ENGINE_NAME}_vs_${name}.log"
    rm -f "$pgn"

    # Run the pairing; tolerate a non-zero exit (e.g. one stalled game)
    # so a flaky anchor doesn't abort the whole gauntlet.
    set +e
    # shellcheck disable=SC2086
    fastchess \
        -engine cmd="$ENGINE" name="$ENGINE_NAME" \
        -engine cmd="$cmd" name="$name" $opts \
        -each proto=uci tc="$TC" option.Hash=16 \
        -rounds "$ROUNDS" -games 2 -repeat -concurrency "$CONCURRENCY" \
        -openings file="$BOOK" format=epd order=random \
        -draw movenumber=40 movecount=8 score=8 \
        -resign movecount=4 score=800 \
        -pgnout file="$pgn" 2>&1 | tee "$log" | tail -12
    set -e

    # fastchess prints:     "Elo: <value> +/- <err>, nElo: ..."
    # (value/err may be e.g. -137.2, inf, -inf, nan, -nan)
    elo_line="$(grep -E '^Elo: ' "$log" | tail -1 || true)"
    if [[ -z "$elo_line" ]]; then
        echo "WARN: no Elo diff line for $name (match may have been interrupted); skipping"
        continue
    fi
    # shellcheck disable=SC2206
    read -r diff err < <(awk '{
        # Expect tokens: Elo: <diff> +/- <err>, ...
        # Strip trailing comma on field 4.
        e = $4; gsub(",", "", e);
        print $2, e
    }' <<< "$elo_line")

    # fastchess prints +/- nan when the sample is degenerate (e.g. all wins,
    # no draws). Those rows are kept in the per-anchor table but excluded
    # from the inverse-variance combine.
    RESULT_NAMES+=("$name")
    RESULT_ANCHOR_ELO+=("$elo")
    RESULT_ELOS+=("$diff")
    RESULT_ERRS+=("$err")
done

echo
echo "==================================================================="
echo "Per-anchor Elo estimates for $ENGINE_NAME"
echo "==================================================================="
printf "%-10s %10s %10s %14s %14s\n" "anchor" "anchor_elo" "diff" "estimate" "+/- err"
for i in "${!RESULT_NAMES[@]}"; do
    name="${RESULT_NAMES[$i]}"
    anchor_elo="${RESULT_ANCHOR_ELO[$i]}"
    diff="${RESULT_ELOS[$i]}"
    err="${RESULT_ERRS[$i]}"
    est="$(awk -v a="$anchor_elo" -v d="$diff" 'BEGIN{printf "%.1f", a + d}')"
    printf "%-10s %10s %10s %14s %14s\n" "$name" "$anchor_elo" "$diff" "$est" "$err"
done

# Inverse-variance-weighted combination:
#   elo* = sum(w_i * est_i) / sum(w_i),  w_i = 1 / err_i^2
#   err* = 1 / sqrt(sum(w_i))
if [[ ${#RESULT_NAMES[@]} -eq 0 ]]; then
    echo; echo "No anchor results available." >&2; exit 3
fi

combined="$(awk -v n="${#RESULT_NAMES[@]}" \
                -v elos="${RESULT_ELOS[*]}" \
                -v errs="${RESULT_ERRS[*]}" \
                -v anchors="${RESULT_ANCHOR_ELO[*]}" '
    function isnum(s) { return s ~ /^[-+]?[0-9]+(\.[0-9]+)?$/ }
    BEGIN {
        split(elos,    d, " ")
        split(errs,    e, " ")
        split(anchors, a, " ")
        sumw = 0; sumwx = 0; kept = 0
        for (i=1; i<=n; ++i) {
            if (!isnum(e[i]) || !isnum(d[i]) || e[i] <= 0) continue
            est = a[i] + d[i]
            w   = 1.0 / (e[i] * e[i])
            sumw  += w
            sumwx += w * est
            kept  += 1
        }
        if (kept == 0 || sumw == 0) {
            printf "nan nan 0\n"; exit
        }
        elo  = sumwx / sumw
        eerr = 1.0 / sqrt(sumw)
        printf "%.1f %.1f %d\n", elo, eerr, kept
    }
')"

combined_elo="$(echo "$combined" | cut -d' ' -f1)"
combined_err="$(echo "$combined" | cut -d' ' -f2)"
combined_kept="$(echo "$combined" | cut -d' ' -f3)"

echo
echo "==================================================================="
echo "Combined estimate (inverse-variance weighted over $combined_kept anchors):"
echo
if [[ "$combined_elo" == "nan" ]]; then
    echo "  No usable anchors with finite error bars. Re-run with more"
    echo "  rounds per anchor (e.g. ROUNDS=200) so fastchess can produce"
    echo "  non-degenerate samples."
else
    printf "  %s Elo ~= %s +/- %s\n" "$ENGINE_NAME" "$combined_elo" "$combined_err"
fi
echo
echo "Interpretation: absolute Elo on the implicit scale of the anchor"
echo "pool (Stockfish UCI_Elo), at TC=$TC.  Numbers drift with hardware"
echo "and time control - quote them together with both."
echo "==================================================================="
