#!/usr/bin/env bash
# Engine-side bench harness. Runs `engine bench` under several policy
# configurations and prints a Markdown table comparing nodes / NPS /
# wall-clock / bestmove deltas plus the policy latency counters.
#
# Configurations swept:
#   off                   policy disabled (baseline)
#   v1_root_bonus         v1 binary, PolicyMode=1   (= v5.3 default)
#   v1_quiet_residual     v1 binary, PolicyMode=2   (the smallest first change)
#   v2_root_bonus         v2 binary, PolicyV2Mode=1
#   v2_quiet_residual     v2 binary, PolicyV2Mode=2
#
# A configuration is skipped when its binary doesn't exist.
#
# Env:
#   ENGINE        path to engine binary (defaults to ./build/engine)
#   V1_FILE       path to v1 RINPOL1 binary
#   V2_FILE       path to v2 RINPOL2 binary
#   SCALES        comma-separated list of PolicyScale values to sweep
#   QUIET_SCALES  comma-separated list of PolicyV2QuietScale values
#   BONUS_CLAMP   PolicyV2BonusClamp for v2 configs
#   THREADS       1
#   HASH_MB       16

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/chess-engine"

ENGINE="${ENGINE:-./build/engine}"
V1_FILE="${V1_FILE:-data/policy/stockfish-200k/train/smoke-policy.bin}"
V2_FILE="${V2_FILE:-data/policy/v2/policy.bin}"
SCALES="${SCALES:-160}"
QUIET_SCALES="${QUIET_SCALES:-160}"
BONUS_CLAMP="${BONUS_CLAMP:-20000}"
THREADS="${THREADS:-1}"
HASH_MB="${HASH_MB:-16}"

if [[ ! -x "$ENGINE" ]]; then
  echo "engine binary not found at $ENGINE" >&2
  exit 1
fi

run_bench() {
  local label="$1"; shift
  local options=("$@")
  local cmds=("setoption name Threads value $THREADS" "setoption name Hash value $HASH_MB")
  cmds+=("${options[@]}")
  cmds+=("policystats reset" "bench" "policystats")
  local payload
  payload="$(printf '%s\n' "${cmds[@]}")"
  payload+=$'\nquit\n'
  local out
  out="$(printf '%s' "$payload" | "$ENGINE")"
  local nodes nps feature_ns calls
  nodes=$(printf '%s' "$out" | awk '/^Nodes:/ {print $2}' | tail -1)
  nps=$(printf  '%s' "$out" | awk '/^Nodes:/ {print $4}' | tail -1)
  feature_ns=$(printf '%s' "$out" | awk -F'feature_ns=' '/policy_calls/ {split($2,a," "); print a[1]}' | tail -1)
  calls=$(printf '%s' "$out" | awk -F'policy_calls=' '/policy_calls/ {split($2,a," "); print a[1]}' | tail -1)
  printf '| %s | %s | %s | %s | %s |\n' "$label" "${nodes:-?}" "${nps:-?}" "${feature_ns:-0}" "${calls:-0}"
}

echo "| config | nodes | nps | feature_ns | policy_calls |"
echo "|--------|------:|----:|-----------:|-------------:|"

# Baseline: policy off entirely.
run_bench "off" "setoption name UsePolicy value false" "setoption name UsePolicyV2 value false"

# v1 sweeps.
if [[ -f "$V1_FILE" ]]; then
  IFS=',' read -ra arr_scales <<<"$SCALES"
  for scale in "${arr_scales[@]}"; do
    run_bench "v1_root_bonus_s${scale}" \
      "setoption name UsePolicyV2 value false" \
      "setoption name PolicyFile value $V1_FILE" \
      "setoption name UsePolicy value true" \
      "setoption name PolicyMode value 1" \
      "setoption name PolicyScale value $scale"
    run_bench "v1_quiet_residual_s${scale}" \
      "setoption name UsePolicyV2 value false" \
      "setoption name PolicyFile value $V1_FILE" \
      "setoption name UsePolicy value true" \
      "setoption name PolicyMode value 2" \
      "setoption name PolicyScale value $scale"
  done
fi

# v2 sweeps.
if [[ -f "$V2_FILE" ]]; then
  IFS=',' read -ra arr_scales <<<"$SCALES"
  IFS=',' read -ra arr_qscales <<<"$QUIET_SCALES"
  for scale in "${arr_scales[@]}"; do
    for qscale in "${arr_qscales[@]}"; do
      run_bench "v2_root_bonus_s${scale}_q${qscale}" \
        "setoption name UsePolicy value false" \
        "setoption name PolicyV2File value $V2_FILE" \
        "setoption name UsePolicyV2 value true" \
        "setoption name PolicyV2Mode value 1" \
        "setoption name PolicyV2Scale value $scale" \
        "setoption name PolicyV2QuietScale value $qscale" \
        "setoption name PolicyV2BonusClamp value $BONUS_CLAMP"
      run_bench "v2_quiet_residual_s${scale}_q${qscale}" \
        "setoption name UsePolicy value false" \
        "setoption name PolicyV2File value $V2_FILE" \
        "setoption name UsePolicyV2 value true" \
        "setoption name PolicyV2Mode value 2" \
        "setoption name PolicyV2Scale value $scale" \
        "setoption name PolicyV2QuietScale value $qscale" \
        "setoption name PolicyV2BonusClamp value $BONUS_CLAMP"
    done
  done
fi
