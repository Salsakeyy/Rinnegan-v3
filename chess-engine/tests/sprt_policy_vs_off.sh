#!/usr/bin/env bash
#
# SPRT match: same engine binary on both sides, distinguished only by
# UCI policy options. Used to validate whether the fast v1 policy is a
# wall-clock Elo win against UsePolicy=false.
#
# Default candidate (env-overridable):
#   UsePolicy=true PolicyMode=1 PolicyScale=160 PolicyFile=<path>
# Default baseline:
#   UsePolicy=false
#
# Usage:
#   bash tests/sprt_policy_vs_off.sh                       # run with defaults
#   ROUNDS=2000 SCALE=120 bash tests/sprt_policy_vs_off.sh # custom
#   bash tests/sprt_policy_vs_off.sh --gauntlet            # disable SPRT, fixed game count
#
# Env knobs:
#   ENGINE        path to engine binary (default build/engine)
#   POLICY_FILE   path to v1 RINPOL1 binary
#   SCALE         PolicyScale (default 160)
#   QUIET_SCALE   PolicyV2QuietScale for v2 only (default SCALE)
#   MODE          PolicyMode  (default 1 = root_bonus)
#   ENDGAME_SCALE PolicyV2EndgameScale percent for v2 only (default 100)
#   BONUS_CLAMP   PolicyV2BonusClamp for v2 only (default 20000)
#   ROUNDS        rounds (games = 2 * rounds), default 1000
#   TC            time control, default 10+0.1
#   HASH          hash MB, default 16
#   CONCURRENCY   parallel games (default min(nproc/2, 4))
#   ELO0 / ELO1   SPRT bounds (default 0 / 5)
#   ALPHA / BETA  SPRT errors (default 0.05 / 0.05)
#   OUT_DIR       results directory (default data/sprt/<tag>)
#
# Output:
#   $OUT_DIR/games.pgn   PGN of all games
#   $OUT_DIR/run.log     fastchess summary
#   $OUT_DIR/cmd.txt     exact command used (reproducibility)
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT"

ENGINE="${ENGINE:-$ROOT/build/engine}"
POLICY_FILE="${POLICY_FILE:-$ROOT/data/policy/new32-10m-f32-h512-runpod/h512/train/smoke-policy.bin}"
SCALE="${SCALE:-160}"
QUIET_SCALE="${QUIET_SCALE:-$SCALE}"
MODE="${MODE:-1}"
ENDGAME_SCALE="${ENDGAME_SCALE:-100}"
BONUS_CLAMP="${BONUS_CLAMP:-20000}"
ROUNDS="${ROUNDS:-1000}"
TC="${TC:-10+0.1}"
HASH="${HASH:-16}"
ELO0="${ELO0:-0}"
ELO1="${ELO1:-5}"
ALPHA="${ALPHA:-0.05}"
BETA="${BETA:-0.05}"
GAUNTLET=0

NPROC="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 2)"
CONCURRENCY="${CONCURRENCY:-$((NPROC / 2))}"
[[ "$CONCURRENCY" -lt 1 ]] && CONCURRENCY=1
[[ "$CONCURRENCY" -gt 4 ]] && CONCURRENCY=4

while [[ $# -gt 0 ]]; do
    case "$1" in
        --gauntlet) GAUNTLET=1; shift ;;
        --rounds)   ROUNDS="$2"; shift 2 ;;
        --scale)    SCALE="$2"; shift 2 ;;
        --tc)       TC="$2"; shift 2 ;;
        *)          echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

USE_V2="${USE_V2:-0}"
TAG_PREFIX=$([[ "$USE_V2" -eq 1 ]] && echo "policy_v2" || echo "policy_v1")
TAG="${TAG_PREFIX}_mode${MODE}_s${SCALE}"
if [[ "$USE_V2" -eq 1 ]]; then
    TAG="${TAG}_q${QUIET_SCALE}_eg${ENDGAME_SCALE}_c${BONUS_CLAMP}"
fi
TAG="${TAG}_tc${TC//+/p}"
OUT_DIR="${OUT_DIR:-$ROOT/data/sprt/$TAG}"
mkdir -p "$OUT_DIR"

if [[ ! -x "$ENGINE" ]]; then
    echo "ERROR: engine binary not found at $ENGINE" >&2; exit 1
fi
if [[ ! -f "$POLICY_FILE" ]]; then
    echo "ERROR: policy file not found at $POLICY_FILE" >&2; exit 1
fi
if ! command -v fastchess >/dev/null 2>&1; then
    echo "ERROR: fastchess not found on PATH" >&2; exit 2
fi

BOOK="$HERE/openings.epd"
if [[ ! -f "$BOOK" ]]; then
    echo "ERROR: opening book missing at $BOOK" >&2; exit 1
fi

# fastchess wants per-engine option flags. We use the same binary twice
# and set the policy options only on the candidate side.
ENG_BASE_ARGS=(cmd="$ENGINE" proto=uci option.Hash="$HASH" option.Threads=1)
SPRT_ARGS=()
if [[ "$GAUNTLET" -eq 0 ]]; then
    SPRT_ARGS=(-sprt elo0="$ELO0" elo1="$ELO1" alpha="$ALPHA" beta="$BETA")
fi

if [[ "$USE_V2" -eq 1 ]]; then
    CAND_OPTS=(
        option.PolicyV2File="$POLICY_FILE"
        option.UsePolicyV2=true
        option.PolicyV2Mode="$MODE"
        option.PolicyV2Scale="$SCALE"
        option.PolicyV2QuietScale="$QUIET_SCALE"
        option.PolicyV2EndgameScale="$ENDGAME_SCALE"
        option.PolicyV2BonusClamp="$BONUS_CLAMP"
    )
    BASE_OPTS=(option.UsePolicyV2=false option.UsePolicy=false)
else
    CAND_OPTS=(
        option.PolicyFile="$POLICY_FILE"
        option.UsePolicy=true
        option.PolicyMode="$MODE"
        option.PolicyScale="$SCALE"
    )
    BASE_OPTS=(option.UsePolicy=false)
fi

CMD=(
    fastchess
    -engine "${ENG_BASE_ARGS[@]}" name="${TAG_PREFIX}_m${MODE}_s${SCALE}"
        "${CAND_OPTS[@]}"
    -engine "${ENG_BASE_ARGS[@]}" name=policy_off
        "${BASE_OPTS[@]}"
    -each tc="$TC"
    -rounds "$ROUNDS" -games 2 -repeat -concurrency "$CONCURRENCY"
    -openings file="$BOOK" format=epd order=random
    -draw movenumber=40 movecount=8 score=8
    -resign movecount=4 score=800
    ${SPRT_ARGS[@]+"${SPRT_ARGS[@]}"}
    -pgnout file="$OUT_DIR/games.pgn"
)

# Save the exact command for reproducibility.
{
    printf '%q ' "${CMD[@]}"
    echo
} > "$OUT_DIR/cmd.txt"

echo "Tag             : $TAG"
echo "Engine          : $ENGINE"
echo "Policy file     : $POLICY_FILE"
echo "Mode / scale    : $MODE / $SCALE"
if [[ "$USE_V2" -eq 1 ]]; then
    echo "Quiet scale     : $QUIET_SCALE"
    echo "Endgame scale   : $ENDGAME_SCALE"
    echo "Bonus clamp     : $BONUS_CLAMP"
fi
echo "Time control    : $TC"
echo "Rounds (max)    : $ROUNDS  → games up to $((ROUNDS * 2))"
echo "Concurrency     : $CONCURRENCY"
echo "SPRT            : $([[ "$GAUNTLET" -eq 0 ]] && echo "elo0=$ELO0 elo1=$ELO1 alpha=$ALPHA beta=$BETA" || echo "off (gauntlet mode)")"
echo "Output dir      : $OUT_DIR"
echo

# Tee output so the user can `tail -f run.log` while it's running.
"${CMD[@]}" 2>&1 | tee "$OUT_DIR/run.log"
