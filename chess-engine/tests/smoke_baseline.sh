#!/usr/bin/env bash
#
# Classical baseline guardrail for local patches and release candidates.
# Runs fast checks that should pass before longer SPRT or gauntlet testing.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${1:-$ROOT/build/engine}"
PERFT="${2:-$ROOT/build/perft}"
EXPECTED_BENCH="${EXPECTED_BENCH:-3136877}"

[[ -x "$ENGINE" ]] || { echo "ERROR: engine not executable: $ENGINE" >&2; exit 1; }
[[ -x "$PERFT" ]] || { echo "ERROR: perft not executable: $PERFT" >&2; exit 1; }

echo "[smoke] perft"
"$PERFT" | tee /tmp/rinnegan-perft.$$ | grep -q "6/6 tests passed"
rm -f /tmp/rinnegan-perft.$$

echo "[smoke] uci/isready/options"
uci_output="$(printf 'uci\nisready\nquit\n' | "$ENGINE")"
grep -q "uciok" <<<"$uci_output"
grep -q "readyok" <<<"$uci_output"
grep -q "option name Clear Hash type button" <<<"$uci_output"
grep -q "option name MoveOverhead type spin default 25" <<<"$uci_output"
grep -q "option name UsePolicy type check default false" <<<"$uci_output"

echo "[smoke] evaltrace"
printf 'position startpos\nevaltrace\nquit\n' | "$ENGINE" | grep -q "info string eval_trace"

echo "[smoke] fixed-node search"
printf 'position startpos\ngo nodes 20000\nquit\n' | "$ENGINE" | grep -q "^bestmove "

echo "[smoke] root searchmoves"
printf 'position startpos\ngo searchmoves e2e4 depth 1\nquit\n' | "$ENGINE" | grep -q "^bestmove e2e4"

echo "[smoke] bench signature"
bench_output="$(printf 'bench\nquit\n' | "$ENGINE")"
bench_nodes="$(awk '/^Nodes:/ { print $2 }' <<<"$bench_output")"
if [[ "$bench_nodes" != "$EXPECTED_BENCH" ]]; then
    echo "$bench_output"
    echo "ERROR: bench signature mismatch: expected $EXPECTED_BENCH got ${bench_nodes:-missing}" >&2
    exit 1
fi

echo "[smoke] ok"
