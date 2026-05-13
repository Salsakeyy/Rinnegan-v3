#!/usr/bin/env python3
"""Label FENs with any UCI engine."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

from common import iter_fens, iter_jsonl, open_text


SCORE_RE = re.compile(r"\bscore\s+(cp|mate)\s+(-?\d+)")
DEPTH_RE = re.compile(r"\bdepth\s+(\d+)")


class UCIEngine:
    def __init__(self, executable: Path):
        self.executable = executable
        self.proc = subprocess.Popen(
            [str(executable)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        if self.proc.stdin is None or self.proc.stdout is None:
            raise RuntimeError("failed to open engine pipes")

    def close(self) -> None:
        if self.proc.poll() is None:
            try:
                self.send("quit")
                self.proc.wait(timeout=2)
            except Exception:
                self.proc.kill()

    def send(self, command: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(command + "\n")
        self.proc.stdin.flush()

    def read_line(self) -> str:
        assert self.proc.stdout is not None
        line = self.proc.stdout.readline()
        if line == "" and self.proc.poll() is not None:
            raise RuntimeError(f"engine exited with code {self.proc.returncode}")
        return line.rstrip("\n")

    def wait_for(self, token: str) -> list[str]:
        lines: list[str] = []
        while True:
            line = self.read_line()
            lines.append(line)
            if line == token:
                return lines

    def initialize(self, options: dict[str, str]) -> None:
        self.send("uci")
        self.wait_for("uciok")
        for name, value in options.items():
            self.send(f"setoption name {name} value {value}")
        self.send("isready")
        self.wait_for("readyok")

    def analyse(self, fen: str, depth: int, nodes: int, movetime: int) -> dict:
        self.send(f"position fen {fen}")
        if nodes > 0:
            self.send(f"go nodes {nodes}")
        elif movetime > 0:
            self.send(f"go movetime {movetime}")
        else:
            self.send(f"go depth {depth}")

        best_score: tuple[str, int] | None = None
        best_depth = 0
        pv = ""
        bestmove = ""
        while True:
            line = self.read_line()
            if line.startswith("bestmove"):
                parts = line.split()
                if len(parts) >= 2:
                    bestmove = parts[1]
                break
            score_match = SCORE_RE.search(line)
            if score_match:
                best_score = (score_match.group(1), int(score_match.group(2)))
                depth_match = DEPTH_RE.search(line)
                if depth_match:
                    best_depth = int(depth_match.group(1))
                pv_idx = line.find(" pv ")
                if pv_idx >= 0:
                    pv = line[pv_idx + 4 :]

        if best_score is None:
            raise RuntimeError(f"no score returned for {fen}")

        kind, value = best_score
        if kind == "mate":
            sign = 1 if value > 0 else -1
            score = sign * (30000 - min(abs(value), 200))
            mate = value
        else:
            score = value
            mate = None

        return {"score": int(score), "mate": mate, "depth": best_depth, "pv": pv, "bestmove": bestmove}


def parse_option(values: list[str]) -> dict[str, str]:
    options: dict[str, str] = {}
    for item in values:
        if "=" not in item:
            raise ValueError(f"option must be NAME=VALUE, got {item!r}")
        name, value = item.split("=", 1)
        options[name] = value
    return options


def profile_options(profile: str, threads: int, hash_mb: int) -> dict[str, str]:
    base = {"Threads": str(threads), "Hash": str(hash_mb)}
    if profile in ("none", "rinnegan", "stockfish"):
        return base
    if profile == "rinnegan-classical":
        return base
    raise ValueError(f"unknown teacher profile: {profile}")


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
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--teacher-profile", default="rinnegan",
                        choices=["none", "rinnegan", "rinnegan-classical", "stockfish"])
    parser.add_argument("--depth", type=int, default=8)
    parser.add_argument("--nodes", type=int, default=0)
    parser.add_argument("--movetime", type=int, default=0)
    parser.add_argument("--threads", type=int, default=max(1, os.cpu_count() or 1))
    parser.add_argument("--hash", type=int, default=1024)
    parser.add_argument("--option", action="append", default=[], help="Extra UCI option as NAME=VALUE")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--resume", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--flush-every", type=int, default=64)
    args = parser.parse_args()

    if not args.engine.exists():
        raise FileNotFoundError(args.engine)

    options = profile_options(args.teacher_profile, args.threads, args.hash)
    options.update(parse_option(args.option))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    done = read_done(args.output) if args.resume else set()
    mode = "at" if args.resume else "wt"

    fens = list(iter_fens(args.input))
    if args.limit > 0:
        fens = fens[: args.limit]

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
                    result = engine.analyse(fen, args.depth, args.nodes, args.movetime)
                except Exception as exc:
                    print(f"warning: failed to label index {idx}: {exc}", file=sys.stderr)
                    continue

                record = {
                    "fen": fen,
                    "score": result["score"],
                    "mate": result["mate"],
                    "depth": result["depth"],
                    "pv": result["pv"],
                    "bestmove": result["bestmove"],
                    "teacher": args.teacher_profile,
                    "engine": str(args.engine),
                }
                handle.write(json.dumps(record, separators=(",", ":")) + "\n")
                labeled += 1
                if labeled % args.flush_every == 0:
                    handle.flush()
                if labeled % max(args.flush_every, 1) == 0:
                    elapsed = max(time.time() - started, 1e-6)
                    print(f"labeled {labeled} new / {idx} seen ({labeled / elapsed:.2f}/s)", file=sys.stderr)
    finally:
        engine.close()

    print(f"wrote {labeled} new labels to {args.output}; skipped {skipped} existing")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
