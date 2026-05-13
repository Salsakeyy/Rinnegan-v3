#!/usr/bin/env bash
set -euo pipefail

: "${SSH_TARGET:?set SSH_TARGET, e.g. root@213.173.108.12}"
: "${SSH_PORT:?set SSH_PORT, e.g. 17445}"
: "${SSH_KEY:?set SSH_KEY, e.g. ~/.ssh/id_ed25519}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE="${BUNDLE:-$ROOT/runpod-transfer/rinnegan-chess-engine-runpod.tgz}"
LABELS="${LABELS:-/Users/lorenzobodmer/Desktop/Ideas/rinnegan-policy-cloud/data/rinnegan_depth8_labels.jsonl.gz}"
REMOTE_ROOT="${REMOTE_ROOT:-/workspace/rinnegan-policy}"
TRAIN_SCRIPT="${TRAIN_SCRIPT:-tools/policy/run_runpod_new64_train.sh}"
REMOTE_OUT="${REMOTE_OUT:-$REMOTE_ROOT/new64-10m-f32}"

SSH=(ssh -p "$SSH_PORT" -i "$SSH_KEY" -o StrictHostKeyChecking=accept-new "$SSH_TARGET")
SCP=(scp -P "$SSH_PORT" -i "$SSH_KEY" -o StrictHostKeyChecking=accept-new)

echo "== create remote dirs =="
"${SSH[@]}" "mkdir -p '$REMOTE_ROOT/data'"

echo "== upload code bundle =="
"${SCP[@]}" "$BUNDLE" "$SSH_TARGET:$REMOTE_ROOT/"

echo "== upload compressed labels =="
"${SCP[@]}" "$LABELS" "$SSH_TARGET:$REMOTE_ROOT/data/rinnegan_depth8_labels.jsonl.gz"

echo "== unpack code =="
"${SSH[@]}" "cd '$REMOTE_ROOT' && tar -xzf rinnegan-chess-engine-runpod.tgz"

echo "== start detached training =="
"${SSH[@]}" "mkdir -p '$REMOTE_OUT' && cd '$REMOTE_ROOT/chess-engine' && nohup env OUT='$REMOTE_OUT' LABELS='$REMOTE_ROOT/data/rinnegan_depth8_labels.jsonl.gz' bash '$TRAIN_SCRIPT' > '$REMOTE_OUT/run.log' 2>&1 & echo \\$! > '$REMOTE_OUT/pid'"

echo "remote log:"
echo "ssh -p $SSH_PORT -i $SSH_KEY $SSH_TARGET 'tail -f $REMOTE_OUT/run.log'"
