# Rinnegan NNUE Training Tools

These scripts build a first Rinnegan-compatible NNUE from Stockfish labels.
They are intentionally resumable because depth-40 Stockfish labeling for
100k-200k positions can run for a long time.

## Install Python dependencies

```sh
cd chess-engine
python3 -m pip install -r tools/nnue/requirements.txt
```

## One-command depth-40 experiment

```sh
cd chess-engine
COUNT=200000 DEPTH=40 bash tools/nnue/run_depth40_experiment.sh
```

Defaults:

- downloads the latest official Stockfish development pre-release;
- uses Stockfish's built-in/default NNUE evaluation;
- searches every labeled position to depth 40;
- gives Stockfish all logical CPU threads;
- derives a large hash from installed RAM;
- trains the existing `(768 -> 768) x 2 -> 1` SCReLU architecture;
- initializes from `rinnegan-v4.net` when that file exists.

The wrapper refreshes the Stockfish binary by default. Set `STOCKFISH_FORCE=0`
to reuse an existing `tools/nnue/bin/stockfish`.

For a stable Stockfish release instead of the latest development pre-release:

```sh
STOCKFISH_CHANNEL=stable COUNT=100000 bash tools/nnue/run_depth40_experiment.sh
```

## Manual pipeline

Download Stockfish:

```sh
python3 tools/nnue/stockfish_setup.py --channel dev
```

Generate candidate positions:

```sh
python3 tools/nnue/make_positions.py \
  --out data/nnue/candidates.fens \
  --count 200000
```

Label with Stockfish depth 40:

```sh
python3 tools/nnue/label_with_stockfish.py \
  --input data/nnue/candidates.fens \
  --out data/nnue/stockfish-depth40.jsonl \
  --stockfish tools/nnue/bin/stockfish \
  --depth 40 \
  --limit 200000
```

Train and export a drop-in raw net:

```sh
python3 tools/nnue/train_rinnegan_nnue.py \
  --data data/nnue/stockfish-depth40.jsonl \
  --out data/nnue/rinnegan-depth40-200k.net \
  --init-net rinnegan-v4.net
```

Test it in Rinnegan:

```sh
printf 'uci\nsetoption name EvalFile value data/nnue/rinnegan-depth40-200k.net\nisready\nbench\nquit\n' \
  | ./build/engine
```

## Resource controls

`label_with_stockfish.py` defaults to all logical CPUs. To leave headroom:

```sh
python3 tools/nnue/label_with_stockfish.py --threads 8 --hash-mb 8192
```

The generated data and nets live under `data/nnue/`, which is ignored by git.
