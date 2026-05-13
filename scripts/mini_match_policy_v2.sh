#!/usr/bin/env bash
# Short paired gauntlet for one v2 policy config before committing to SPRT.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/chess-engine"

POLICY_FILE="${POLICY_FILE:-data/policy/v2/run-stockfish-mpv8-d14d15-400k-h256/policy_eg10.bin}"
MODE="${MODE:-2}"
SCALE="${SCALE:-400}"
QUIET_SCALE="${QUIET_SCALE:-$SCALE}"
ENDGAME_SCALE="${ENDGAME_SCALE:-0}"
BONUS_CLAMP="${BONUS_CLAMP:-20000}"
ROUNDS="${ROUNDS:-50}"
TC="${TC:-10+0.1}"
HASH="${HASH:-16}"
CONCURRENCY="${CONCURRENCY:-4}"
TAG="${TAG:-policy_v2_m${MODE}_s${SCALE}_q${QUIET_SCALE}_eg${ENDGAME_SCALE}_c${BONUS_CLAMP}_mini_tc${TC//+/p}}"
OUT_DIR="${OUT_DIR:-$ROOT/chess-engine/data/minimatch/$TAG}"

USE_V2=1 \
POLICY_FILE="$POLICY_FILE" \
MODE="$MODE" \
SCALE="$SCALE" \
QUIET_SCALE="$QUIET_SCALE" \
ENDGAME_SCALE="$ENDGAME_SCALE" \
BONUS_CLAMP="$BONUS_CLAMP" \
ROUNDS="$ROUNDS" \
TC="$TC" \
HASH="$HASH" \
CONCURRENCY="$CONCURRENCY" \
OUT_DIR="$OUT_DIR" \
bash tests/sprt_policy_vs_off.sh --gauntlet
