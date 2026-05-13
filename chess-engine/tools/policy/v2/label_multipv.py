#!/usr/bin/env python3
"""MultiPV-aware policy labeler.

Three teacher modes:

  stockfish_multipv      Stockfish (any UCI engine) at MultiPV K. Each
                         FEN produces K {move, cp/mate} entries plus the
                         eventual bestmove.
  rinnegan_deep_multipv  Same shape, but with a Rinnegan binary as the
                         teacher and `setoption name MultiPV value K`.
                         Useful when an external SF is not available
                         (the engine has supported MultiPV since v3).
  hard_bestmove_fallback Single bestmove per FEN; identical record shape
                         to v1 `label_uci.py` but written through the
                         v2 codepath so the packer can route both modes.

Output is JSONL. One record per FEN:

  {
    "fen": "...",
    "teacher_mode": "stockfish_multipv",
    "engine": "/path/to/teacher",
    "depth": 18,
    "nodes": 0,
    "movetime": 0,
    "multipv": [
      {"move": "e2e4", "cp": 25,  "mate": null, "depth": 18, "rank": 1},
      {"move": "d2d4", "cp": 18,  "mate": null, "depth": 18, "rank": 2},
      ...
    ],
    "bestmove": "e2e4"
  }

The script is intentionally resumable: if the output already contains a
record for a FEN, it is skipped on restart.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from common import iter_fens, iter_jsonl, open_text  # noqa: E402

SCORE_RE = re.compile(r"\bscore\s+(cp|mate)\s+(-?\d+)")
DEPTH_RE = re.compile(r"\bdepth\s+(\d+)")
MULTIPV_RE = re.compile(r"\bmultipv\s+(\d+)")


class UCIEngine:
    def __init__(self, executable: Path):
        self.executable = executable
        self.proc = subprocess.Popen(
            [str(executable)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1,
        )
        if self.proc.stdin is None or self.proc.stdout is None:
            raise RuntimeError("failed to open engine pipes")

    def close(self) -> None:
        if self.proc.poll() is None:
            try:
                self.send("quit"); self.proc.wait(timeout=2)
            except Exception:
                self.proc.kill()

    def send(self, cmd: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(cmd + "\n"); self.proc.stdin.flush()

    def read_line(self) -> str:
        assert self.proc.stdout is not None
        line = self.proc.stdout.readline()
        if line == "" and self.proc.poll() is not None:
            raise RuntimeError(f"engine exited with code {self.proc.returncode}")
        return line.rstrip("\n")

    def wait_for(self, token: str) -> None:
        while True:
            if self.read_line() == token:
                return

    def initialize(self, options: dict[str, str]) -> None:
        self.send("uci"); self.wait_for("uciok")
        for name, value in options.items():
            self.send(f"setoption name {name} value {value}")
        self.send("isready"); self.wait_for("readyok")

    def analyse(self, fen: str, depth: int, nodes: int, movetime: int, multipv: int) -> dict:
        self.send(f"position fen {fen}")
        if multipv > 1:
            # Some engines require MultiPV to be (re-)set per position.
            self.send(f"setoption name MultiPV value {multipv}")
        if nodes > 0:
            self.send(f"go nodes {nodes}")
        elif movetime > 0:
            self.send(f"go movetime {movetime}")
        else:
            self.send(f"go depth {depth}")

        # Latest info-line per (multipv-rank). Replaced as deeper info
        # arrives. Final bestmove appended at the end.
        latest: dict[int, dict] = {}
        bestmove = ""
        while True:
            line = self.read_line()
            if line.startswith("bestmove"):
                parts = line.split()
                if len(parts) >= 2:
                    bestmove = parts[1]
                break
            score_match = SCORE_RE.search(line)
            if not score_match:
                continue
            kind, raw_value = score_match.group(1), int(score_match.group(2))
            depth_match = DEPTH_RE.search(line)
            if not depth_match:
                continue
            line_depth = int(depth_match.group(1))
            multipv_match = MULTIPV_RE.search(line)
            rank = int(multipv_match.group(1)) if multipv_match else 1
            pv_idx = line.find(" pv ")
            if pv_idx < 0:
                continue
            pv = line[pv_idx + 4:].strip().split()
            if not pv:
                continue
            move = pv[0]
            cp: float | None = float(raw_value) if kind == "cp" else None
            mate = int(raw_value) if kind == "mate" else None
            latest[rank] = {
                "move": move,
                "cp": cp,
                "mate": mate,
                "depth": line_depth,
                "rank": rank,
            }

        if not latest and not bestmove:
            raise RuntimeError(f"no score or bestmove returned for {fen}")

        # Sort by rank (1 first). Some engines re-rank only after extra
        # iterations; trust whatever rank=1 points to.
        ordered = [latest[k] for k in sorted(latest.keys())]
        if not bestmove and ordered:
            bestmove = ordered[0]["move"]
        return {"multipv": ordered, "bestmove": bestmove}


def parse_kv(values: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for v in values:
        if "=" not in v:
            raise ValueError(f"option must be NAME=VALUE, got {v!r}")
        name, value = v.split("=", 1)
        out[name] = value
    return out


def read_done(path: Path) -> set[str]:
    if not path.exists():
        return set()
    done: set[str] = set()
    for record in iter_jsonl(path):
        fen = record.get("fen")
        if fen:
            done.add(fen)
    return done


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--input",  type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--teacher-mode", required=True,
                        choices=["stockfish_multipv", "rinnegan_deep_multipv", "hard_bestmove_fallback"])
    parser.add_argument("--multipv", type=int, default=8)
    parser.add_argument("--depth",   type=int, default=18)
    parser.add_argument("--nodes",   type=int, default=0)
    parser.add_argument("--movetime",type=int, default=0)
    parser.add_argument("--threads", type=int, default=max(1, os.cpu_count() or 1))
    parser.add_argument("--hash",    type=int, default=1024)
    parser.add_argument("--option", action="append", default=[],
                        help="Extra UCI option as NAME=VALUE (repeatable)")
    parser.add_argument("--limit",   type=int, default=0)
    parser.add_argument("--resume", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--flush-every", type=int, default=64)
    args = parser.parse_args()

    if not args.engine.exists():
        raise FileNotFoundError(args.engine)

    if args.teacher_mode == "hard_bestmove_fallback":
        args.multipv = 1

    options: dict[str, str] = {
        "Threads": str(args.threads),
        "Hash":    str(args.hash),
    }
    if args.multipv > 1:
        options["MultiPV"] = str(args.multipv)
    options.update(parse_kv(args.option))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    done = read_done(args.output) if args.resume else set()
    mode = "at" if args.resume else "wt"

    fens = list(iter_fens(args.input))
    if args.limit > 0:
        fens = fens[:args.limit]

    engine = UCIEngine(args.engine)
    labeled = 0
    skipped = 0
    started = time.time()
    try:
        engine.initialize(options)
        with open_text(args.output, mode) as handle:
            for idx, fen in enumerate(fens, start=1):
                if fen in done:
                    skipped += 1
                    continue
                try:
                    res = engine.analyse(fen, args.depth, args.nodes, args.movetime, args.multipv)
                except Exception as exc:
                    print(f"warning: failed to label index {idx}: {exc}", file=sys.stderr)
                    # If the engine subprocess died (e.g. SIGABRT on a poison
                    # FEN), restart it so the rest of this chunk can finish.
                    # Without this, every subsequent FEN fails with broken pipe.
                    if engine.proc.poll() is not None:
                        print("info: engine died, restarting", file=sys.stderr)
                        try: engine.close()
                        except Exception: pass
                        engine = UCIEngine(args.engine)
                        engine.initialize(options)
                    continue
                record = {
                    "fen": fen,
                    "teacher_mode": args.teacher_mode,
                    "engine": str(args.engine),
                    "depth": args.depth,
                    "nodes": args.nodes,
                    "movetime": args.movetime,
                    "multipv": res["multipv"],
                    "bestmove": res["bestmove"],
                }
                handle.write(json.dumps(record, separators=(",", ":")) + "\n")
                labeled += 1
                if labeled % args.flush_every == 0:
                    handle.flush()
                    elapsed = max(time.time() - started, 1e-6)
                    print(f"labeled {labeled} new / {idx} seen ({labeled / elapsed:.2f}/s)",
                          file=sys.stderr)
    finally:
        engine.close()

    print(f"wrote {labeled} new labels to {args.output}; skipped {skipped} existing")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
