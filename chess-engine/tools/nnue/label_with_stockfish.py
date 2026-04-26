#!/usr/bin/env python3
"""Label FEN positions with official Stockfish.

Defaults are intentionally heavy: depth 40, all logical CPU threads, and a large
hash sized from installed RAM. The output is JSONL and resumable.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_STOCKFISH = Path(__file__).resolve().parent / "bin" / (
    "stockfish.exe" if platform.system() == "Windows" else "stockfish"
)


@dataclass
class UciOption:
    name: str
    kind: str | None = None
    min_value: int | None = None
    max_value: int | None = None


def total_memory_mb() -> int | None:
    system = platform.system()
    if system == "Darwin":
        try:
            raw = subprocess.check_output(["sysctl", "-n", "hw.memsize"], text=True).strip()
            return int(raw) // (1024 * 1024)
        except (OSError, subprocess.CalledProcessError, ValueError):
            return None
    if system == "Linux":
        try:
            for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
                if line.startswith("MemTotal:"):
                    return int(line.split()[1]) // 1024
        except (OSError, ValueError):
            return None
    return None


def default_hash_mb(ratio: float) -> int:
    mem = total_memory_mb()
    if not mem:
        return 4096
    reserve = max(2048, int(mem * 0.12))
    usable = max(16, mem - reserve)
    return max(16, int(usable * ratio))


def resolve_stockfish(path: Path | None) -> Path:
    if path:
        return path
    if DEFAULT_STOCKFISH.exists():
        return DEFAULT_STOCKFISH
    found = shutil.which("stockfish")
    if found:
        return Path(found)
    raise SystemExit(
        "Stockfish not found. Run `python3 tools/nnue/stockfish_setup.py` "
        "or pass --stockfish /path/to/stockfish."
    )


def parse_option(line: str) -> UciOption | None:
    tokens = line.split()
    if len(tokens) < 5 or tokens[0] != "option" or tokens[1] != "name":
        return None
    try:
        type_idx = tokens.index("type")
    except ValueError:
        return None
    name = " ".join(tokens[2:type_idx])
    kind = tokens[type_idx + 1] if type_idx + 1 < len(tokens) else None
    opt = UciOption(name=name, kind=kind)
    for key, attr in (("min", "min_value"), ("max", "max_value")):
        if key in tokens:
            idx = tokens.index(key)
            if idx + 1 < len(tokens):
                try:
                    setattr(opt, attr, int(tokens[idx + 1]))
                except ValueError:
                    pass
    return opt


class UciEngine:
    def __init__(self, path: Path):
        self.path = path
        self.proc = subprocess.Popen(
            [str(path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        self.options: dict[str, UciOption] = {}
        self.id_name = ""

    def close(self) -> None:
        if self.proc.poll() is not None:
            return
        try:
            self.send("quit")
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def send(self, command: str) -> None:
        if self.proc.stdin is None:
            raise RuntimeError("Stockfish stdin is closed")
        self.proc.stdin.write(command + "\n")
        self.proc.stdin.flush()

    def read_line(self) -> str:
        if self.proc.stdout is None:
            raise RuntimeError("Stockfish stdout is closed")
        line = self.proc.stdout.readline()
        if line == "":
            raise RuntimeError("Stockfish exited unexpectedly")
        return line.rstrip("\n")

    def init(self) -> None:
        self.send("uci")
        while True:
            line = self.read_line()
            if line.startswith("id name "):
                self.id_name = line[len("id name ") :]
            elif line.startswith("option name "):
                opt = parse_option(line)
                if opt:
                    self.options[opt.name.lower()] = opt
            elif line == "uciok":
                break

    def has_option(self, name: str) -> bool:
        return name.lower() in self.options

    def option_name(self, name: str) -> str:
        return self.options[name.lower()].name

    def set_option(self, name: str, value: str | int | bool) -> bool:
        if not self.has_option(name):
            return False
        exact = self.option_name(name)
        if isinstance(value, bool):
            value = "true" if value else "false"
        self.send(f"setoption name {exact} value {value}")
        return True

    def set_spin(self, name: str, value: int) -> int | None:
        if not self.has_option(name):
            return None
        opt = self.options[name.lower()]
        if opt.min_value is not None:
            value = max(value, opt.min_value)
        if opt.max_value is not None:
            value = min(value, opt.max_value)
        self.set_option(name, value)
        return value

    def ready(self) -> None:
        self.send("isready")
        while True:
            if self.read_line() == "readyok":
                return

    def configure(
        self,
        threads: int,
        hash_mb: int,
        eval_file: str | None,
        syzygy_path: str | None,
    ) -> dict[str, object]:
        applied: dict[str, object] = {}
        applied["Threads"] = self.set_spin("Threads", threads)
        applied["Hash"] = self.set_spin("Hash", hash_mb)
        if self.has_option("Use NNUE"):
            applied["Use NNUE"] = self.set_option("Use NNUE", True)
        if self.has_option("UCI_AnalyseMode"):
            applied["UCI_AnalyseMode"] = self.set_option("UCI_AnalyseMode", True)
        if self.has_option("UCI_ShowWDL"):
            applied["UCI_ShowWDL"] = self.set_option("UCI_ShowWDL", True)
        if eval_file and self.has_option("EvalFile"):
            applied["EvalFile"] = self.set_option("EvalFile", eval_file)
        if syzygy_path and self.has_option("SyzygyPath"):
            applied["SyzygyPath"] = self.set_option("SyzygyPath", syzygy_path)
        self.ready()
        return applied

    def analyse_depth(self, fen: str, depth: int) -> dict[str, object]:
        self.send(f"position fen {fen}")
        self.send(f"go depth {depth}")
        last: dict[str, object] = {}

        while True:
            line = self.read_line()
            if line.startswith("bestmove "):
                last["bestmove"] = line.split()[1]
                return last
            if not line.startswith("info "):
                continue
            tokens = line.split()
            if "depth" in tokens:
                idx = tokens.index("depth")
                if idx + 1 < len(tokens):
                    last["depth"] = int(tokens[idx + 1])
            if "seldepth" in tokens:
                idx = tokens.index("seldepth")
                if idx + 1 < len(tokens):
                    last["seldepth"] = int(tokens[idx + 1])
            if "nodes" in tokens:
                idx = tokens.index("nodes")
                if idx + 1 < len(tokens):
                    last["nodes"] = int(tokens[idx + 1])
            if "nps" in tokens:
                idx = tokens.index("nps")
                if idx + 1 < len(tokens):
                    last["nps"] = int(tokens[idx + 1])
            if "wdl" in tokens:
                idx = tokens.index("wdl")
                if idx + 3 < len(tokens):
                    last["wdl"] = [int(tokens[idx + 1]), int(tokens[idx + 2]), int(tokens[idx + 3])]
            if "score" in tokens:
                idx = tokens.index("score")
                if idx + 2 < len(tokens):
                    kind = tokens[idx + 1]
                    value = int(tokens[idx + 2])
                    last["score_kind"] = kind
                    last["score"] = value
            if "pv" in tokens:
                idx = tokens.index("pv")
                last["pv"] = tokens[idx + 1 :]


def read_existing_fens(path: Path) -> set[str]:
    seen: set[str] = set()
    if not path.exists():
        return seen
    with path.open(encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            fen = record.get("fen")
            if isinstance(fen, str):
                seen.add(fen)
    return seen


def iter_fens(path: Path, seen: set[str]) -> list[str]:
    fens: list[str] = []
    with path.open(encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            text = line.strip()
            if not text or text.startswith("#"):
                continue
            fen = text
            if fen not in seen:
                fens.append(fen)
    return fens


def score_to_cp(result: dict[str, object], mate_policy: str, mate_cp: int) -> int | None:
    kind = result.get("score_kind")
    score = result.get("score")
    if not isinstance(kind, str) or not isinstance(score, int):
        return None
    if kind == "cp":
        return score
    if kind == "mate":
        if mate_policy == "skip":
            return None
        sign = 1 if score > 0 else -1
        distance = min(abs(score), 100)
        return sign * (mate_cp - distance)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=ROOT / "data" / "nnue" / "candidates.fens")
    parser.add_argument("--out", type=Path, default=ROOT / "data" / "nnue" / "stockfish-depth40.jsonl")
    parser.add_argument("--stockfish", type=Path)
    parser.add_argument("--depth", type=int, default=40)
    parser.add_argument("--limit", type=int)
    parser.add_argument("--threads", type=int, default=0, help="0 means all logical CPUs")
    parser.add_argument("--hash-mb", type=int, default=0, help="0 means derive a large hash from RAM")
    parser.add_argument("--hash-ratio", type=float, default=0.80)
    parser.add_argument("--eval-file", help="optional Stockfish EvalFile override")
    parser.add_argument("--syzygy-path", help="optional SyzygyPath for Stockfish")
    parser.add_argument("--mate-policy", choices=("skip", "cap"), default="skip")
    parser.add_argument("--mate-cp", type=int, default=32_000)
    parser.add_argument("--max-abs-cp", type=int, default=3_000, help="0 disables extreme score filtering")
    parser.add_argument("--resume", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--log-every", type=int, default=100)
    args = parser.parse_args()

    stockfish = resolve_stockfish(args.stockfish)
    threads = args.threads if args.threads > 0 else (os.cpu_count() or 1)
    hash_mb = args.hash_mb if args.hash_mb > 0 else default_hash_mb(args.hash_ratio)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    seen = read_existing_fens(args.out) if args.resume else set()
    fens = iter_fens(args.input, seen)
    if args.limit is not None:
        fens = fens[: args.limit]

    engine = UciEngine(stockfish)
    started = time.time()
    labeled = 0
    skipped = 0

    try:
        engine.init()
        applied = engine.configure(threads, hash_mb, args.eval_file, args.syzygy_path)
        print(f"Stockfish: {engine.id_name or stockfish}", file=sys.stderr)
        print(f"Applied UCI options: {applied}", file=sys.stderr)
        if not engine.has_option("Use NNUE"):
            print("NNUE: no Use NNUE toggle exposed; using Stockfish's built-in/default neural eval", file=sys.stderr)

        mode = "a" if args.resume else "w"
        with args.out.open(mode, encoding="utf-8") as out:
            for fen in fens:
                result = engine.analyse_depth(fen, args.depth)
                cp = score_to_cp(result, args.mate_policy, args.mate_cp)
                if cp is None:
                    skipped += 1
                    continue
                if args.max_abs_cp > 0 and abs(cp) > args.max_abs_cp:
                    skipped += 1
                    continue
                record = {
                    "fen": fen,
                    "score_cp": cp,
                    "depth_target": args.depth,
                    "depth": result.get("depth"),
                    "seldepth": result.get("seldepth"),
                    "nodes": result.get("nodes"),
                    "nps": result.get("nps"),
                    "wdl": result.get("wdl"),
                    "bestmove": result.get("bestmove"),
                    "pv": result.get("pv"),
                    "engine": engine.id_name,
                }
                out.write(json.dumps(record, separators=(",", ":")) + "\n")
                labeled += 1
                if labeled % args.log_every == 0:
                    elapsed = max(1e-6, time.time() - started)
                    print(
                        f"labeled={labeled} skipped={skipped} rate={labeled / elapsed:.3f}/s",
                        file=sys.stderr,
                        flush=True,
                    )
    finally:
        engine.close()

    print(f"Wrote {labeled} labels to {args.out}; skipped={skipped}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
