# Rinnegan Policy Pipeline

This pipeline trains the root move-ordering policy scorer. The smoke script
still checks that the data path works:

1. generate FENs,
2. label them with a UCI teacher,
3. turn each position into legal move candidates,
4. train a tiny move scorer to rank the teacher move first.

Run from the repository root:

```sh
COUNT=64 DEPTH=2 EPOCHS=4 bash tools/policy/run_smoke_policy.sh
```

Outputs go to `data/policy/smoke` by default. The important file is
`metrics.json`, which reports train and validation top-1/top-3 policy accuracy.

You can also pack existing Stockfish labels:

```sh
LIMIT=200000 EPOCHS=3 bash tools/policy/run_stockfish_policy_sample.sh
```

Train a Rinnegan-labeled quiet root policy from existing FENs:

```sh
LIMIT=250000 NODES=500 TORCH_THREADS=4 DEVICE=cpu \
  bash tools/policy/run_rinnegan_policy_sample.sh
```

For large policy label sets, use the fast path. It writes standalone `.npy`
arrays so the trainer can memory-map the feature matrix instead of loading a
giant `.npz` into RAM, skips the redundant dense labels array, shuffles
contiguous blocks for disk locality, and caps eval work per epoch:

```sh
LABELS=data/policy/labels-10m.jsonl OUT=data/policy/policy-10m-fast \
  ARCHES="384 512" EPOCHS=3 DEVICE=auto \
  bash tools/policy/run_fast_policy_train.sh
```

If the 10M set is already packed as `dataset.npz`, pass it as `DATASET`.
The fast script will stream-extract it into standalone `.npy` files first:

```sh
DATASET=data/policy/policy-10m/dataset.npz OUT=data/policy/policy-10m-fast \
  ARCHES="384 512" EPOCHS=3 DEVICE=auto \
  bash tools/policy/run_fast_policy_train.sh
```

The fast script defaults to the engine-compatible legacy 32-feature layout.
`ARCHES="384 512"` trains two exported nets, `384x384` and `512x512`.
Use `ARCHES=384x512` for one net with a 384-unit first hidden layer and a
512-unit second hidden layer. The fast script defaults `EPOCH_GROUPS=2000000`
for quick iteration; set `EPOCH_GROUPS=0` to sweep the full training split
each epoch.

Compressed label streams can be passed directly:

```sh
LABELS=data/rinnegan_depth8_labels.jsonl.gz FEATURE_LAYOUT=new32 \
  OUT=data/policy/new32-10m-fast ARCHES=384x512 EPOCH_GROUPS=0 \
  bash tools/policy/run_fast_policy_train.sh
```

For disk-limited machines, `FEATURE_DTYPE=f16` stores the feature matrix as
float16 and casts each batch back to float32 during training. This reduces
disk and I/O without changing labels, architecture, optimizer, or exported
runtime weights. The fast script skips FEN/bestmove sidecars by default; set
`WRITE_SIDECARS=1` when those audit files are needed.
