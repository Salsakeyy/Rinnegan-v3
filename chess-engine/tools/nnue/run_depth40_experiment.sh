#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
COUNT="${COUNT:-200000}"
DEPTH="${DEPTH:-40}"
CHANNEL="${STOCKFISH_CHANNEL:-dev}"
DATA_DIR="${DATA_DIR:-$ROOT/data/nnue}"
POSITIONS="$DATA_DIR/candidates.fens"
LABELS="$DATA_DIR/stockfish-depth${DEPTH}.jsonl"
NET_OUT="$DATA_DIR/rinnegan-depth${DEPTH}-${COUNT}.net"
INIT_NET="${INIT_NET:-$ROOT/rinnegan-v4.net}"

cd "$ROOT"

python3 - <<'PY'
import importlib.util
missing = [name for name in ("chess", "torch", "numpy") if importlib.util.find_spec(name) is None]
if missing:
    raise SystemExit(
        "Missing Python packages: "
        + ", ".join(missing)
        + "\nInstall with: python3 -m pip install -r tools/nnue/requirements.txt"
    )
PY

SETUP_ARGS=(--channel "$CHANNEL")
if [[ "${STOCKFISH_FORCE:-1}" != "0" ]]; then
    SETUP_ARGS+=(--force)
fi
python3 tools/nnue/stockfish_setup.py "${SETUP_ARGS[@]}"
python3 tools/nnue/make_positions.py --out "$POSITIONS" --count "$COUNT"
python3 tools/nnue/label_with_stockfish.py \
    --input "$POSITIONS" \
    --out "$LABELS" \
    --depth "$DEPTH" \
    --limit "$COUNT" \
    --stockfish tools/nnue/bin/stockfish

TRAIN_ARGS=(--data "$LABELS" --out "$NET_OUT" --limit "$COUNT")
if [[ -f "$INIT_NET" ]]; then
    TRAIN_ARGS+=(--init-net "$INIT_NET")
fi

python3 tools/nnue/train_rinnegan_nnue.py "${TRAIN_ARGS[@]}"

echo "Net written to $NET_OUT"
