# CCRL Submission Notes

Engine: Rinnegan v5.3
Author: Lorenzo Bodmer
Protocol: UCI
Source: https://github.com/Salsakeyy/Rinnegan-v3

Disclosure: Rinnegan is fully written by Claude and Codex as a proof of
concept.

## Files

- `Rinnegan-v5.3-<platform>` or `Rinnegan-v5.3-<platform>.exe`
- `README_CCRL.txt`
- `source/`

## Recommended CCRL Settings

- Protocol: UCI
- Threads: `1` for 1CPU testing; higher only if testing a multi-CPU list
- Hash: `256` or `512` MB, matching the rest of the tournament
- Ponder: off
- Own book: none
- Learning: none
- Tablebases: not used by the engine

## Build

Portable AVX2 x86_64 release build:

```bash
cmake -S . -B build-ccrl \
  -DCMAKE_BUILD_TYPE=Release \
  -DRINNEGAN_LTO=ON \
  -DRINNEGAN_NATIVE=OFF
cmake --build build-ccrl --target engine perft -j
```

Local package:

```bash
bash tools/package-ccrl.sh
```

## Verification

Expected bench signature for v5.3:

```text
Nodes: 3136877
```

Smoke commands:

```bash
printf 'uci\nisready\nquit\n' | ./Rinnegan-v5.3-<platform>
printf 'bench\nquit\n' | ./Rinnegan-v5.3-<platform>
```
