#!/usr/bin/env python3
"""Stockfish-adjudicate changed root moves from rootset_bench.py."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import shlex
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path
from typing import Any

import chess


CSV_FIELDS = [
    "position_id",
    "fen",
    "config",
    "phase",
    "candidate_move_class",
    "baseline_move_class",
    "baseline_move_policy_rank",
    "baseline_rank_bucket",
    "baseline_move",
    "candidate_move",
    "stockfish_bestmove",
    "stockfish_best_cp",
    "baseline_cp",
    "candidate_cp",
    "baseline_gap_cp",
    "candidate_gap_cp",
    "candidate_cp_gain",
    "candidate_better",
    "sf_depth",
    "full_nodes",
    "baseline_nodes",
    "candidate_nodes",
    "full_wall_ms",
    "baseline_wall_ms",
    "candidate_wall_ms",
]


class UciEngine:
    def __init__(self, engine: Path):
        self.proc = subprocess.Popen(
            [str(engine)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        if self.proc.stdin is None or self.proc.stdout is None:
            raise RuntimeError("failed to open UCI pipes")
        self.stdin = self.proc.stdin
        self.stdout = self.proc.stdout
        self._send("uci")
        self._read_until("uciok")

    def _send(self, line: str) -> None:
        self.stdin.write(line + "\n")
        self.stdin.flush()

    def _readline(self) -> str:
        line = self.stdout.readline()
        if line == "":
            raise RuntimeError("engine exited unexpectedly")
        return line.rstrip("\n")

    def _read_until(self, marker: str) -> list[str]:
        lines: list[str] = []
        while True:
            line = self._readline()
            lines.append(line)
            if line == marker or line.endswith(marker):
                return lines

    def setoption(self, name: str, value: Any | None = None) -> None:
        if value is None:
            self._send(f"setoption name {name}")
        else:
            self._send(f"setoption name {name} value {value}")

    def ready(self) -> None:
        self._send("isready")
        self._read_until("readyok")

    def configure(self, threads: int, hash_mb: int, multipv: int = 1) -> None:
        self.setoption("Threads", threads)
        self.setoption("Hash", hash_mb)
        self.setoption("MultiPV", multipv)
        self.ready()

    def search(
        self,
        fen: str,
        depth: int,
        searchmove: str | None = None,
        clear_hash: bool = True,
    ) -> dict[str, Any]:
        if clear_hash:
            self.setoption("Clear Hash")
        self._send(f"position fen {fen}")
        cmd = f"go depth {depth}"
        if searchmove:
            cmd += f" searchmoves {searchmove}"
        start = time.perf_counter()
        self._send(cmd)

        bestmove = "0000"
        score_cp: int | None = None
        score_mate: int | None = None
        nodes = 0
        nps = 0
        seen_depth = 0
        while True:
            line = self._readline()
            if line.startswith("info "):
                parsed = parse_info_line(line)
                nodes = parsed.get("nodes", nodes)
                nps = parsed.get("nps", nps)
                seen_depth = parsed.get("depth", seen_depth)
                if "score_cp" in parsed:
                    score_cp = parsed["score_cp"]
                    score_mate = None
                if "score_mate" in parsed:
                    score_mate = parsed["score_mate"]
                    score_cp = None
            elif line.startswith("bestmove "):
                fields = line.split()
                if len(fields) >= 2:
                    bestmove = fields[1]
                break

        wall_ms = (time.perf_counter() - start) * 1000.0
        return {
            "bestmove": bestmove,
            "score_cp": score_cp,
            "score_mate": score_mate,
            "score_as_cp": score_as_cp(score_cp, score_mate),
            "nodes": nodes,
            "nps": nps,
            "depth": seen_depth,
            "wall_ms": wall_ms,
        }

    def close(self) -> None:
        if self.proc.poll() is None:
            try:
                self._send("quit")
            except BrokenPipeError:
                pass
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def parse_info_line(line: str) -> dict[str, int]:
    tokens = line.split()
    out: dict[str, int] = {}
    for i, token in enumerate(tokens):
        if token in {"depth", "nodes", "nps", "time"} and i + 1 < len(tokens):
            try:
                out[token] = int(tokens[i + 1])
            except ValueError:
                pass
        if token == "score" and i + 2 < len(tokens):
            try:
                if tokens[i + 1] == "cp":
                    out["score_cp"] = int(tokens[i + 2])
                elif tokens[i + 1] == "mate":
                    out["score_mate"] = int(tokens[i + 2])
            except ValueError:
                pass
    return out


def score_as_cp(cp: int | None, mate: int | None) -> int | None:
    if cp is not None:
        return cp
    if mate is None:
        return None
    sign = 1 if mate > 0 else -1
    return sign * (30000 - min(abs(mate), 999))


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for raw in handle:
            line = raw.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def rank_bucket(value: Any) -> str:
    if value in (None, ""):
        return "unknown"
    try:
        rank = int(value)
    except (TypeError, ValueError):
        return "unknown"
    if rank <= 1:
        return "1"
    if rank <= 3:
        return "2-3"
    if rank <= 5:
        return "4-5"
    if rank <= 10:
        return "6-10"
    return ">10"


def ratio(num: float, den: float) -> float:
    return num / den if den else 0.0


def pct(value: float) -> str:
    return f"{value * 100.0:.2f}%"


def mean(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def legal_changed_rows(
    rows: list[dict[str, Any]],
    labels: set[str] | None,
    max_changed_per_config: int,
) -> list[dict[str, Any]]:
    counts: dict[str, int] = defaultdict(int)
    selected: list[dict[str, Any]] = []
    for row in rows:
        label = str(row.get("config", ""))
        if label == "off":
            continue
        if labels and label not in labels:
            continue
        if not row.get("changed"):
            continue
        if max_changed_per_config and counts[label] >= max_changed_per_config:
            continue
        try:
            board = chess.Board(str(row["fen"]))
            base = chess.Move.from_uci(str(row["baseline_bestmove"]))
            cand = chess.Move.from_uci(str(row["bestmove"]))
        except Exception:
            continue
        if base not in board.legal_moves or cand not in board.legal_moves:
            continue
        counts[label] += 1
        selected.append(row)
    return selected


def aggregate_root_metrics(rows: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    off_rows = {int(row["position_id"]): row for row in rows if row.get("config") == "off"}
    labels = sorted({str(row["config"]) for row in rows if row.get("config") != "off"})
    out: dict[str, dict[str, Any]] = {}
    for label in labels:
        cfg_rows = [row for row in rows if row.get("config") == label]
        off_subset = [off_rows[int(row["position_id"])] for row in cfg_rows if int(row["position_id"]) in off_rows]
        cfg_nodes = sum(int(row.get("nodes") or 0) for row in cfg_rows)
        off_nodes = sum(int(row.get("nodes") or 0) for row in off_subset)
        cfg_wall = sum(float(row.get("wall_ms") or 0.0) for row in cfg_rows)
        off_wall = sum(float(row.get("wall_ms") or 0.0) for row in off_subset)
        cfg_nps = cfg_nodes / (cfg_wall / 1000.0) if cfg_wall else 0.0
        off_nps = off_nodes / (off_wall / 1000.0) if off_wall else 0.0
        out[label] = {
            "positions": len(cfg_rows),
            "changes": sum(1 for row in cfg_rows if row.get("changed")),
            "node_ratio": ratio(cfg_nodes, off_nodes),
            "wall_ratio": ratio(cfg_wall, off_wall),
            "nps_ratio": ratio(cfg_nps, off_nps),
        }
    return out


def write_summary(
    path: Path,
    root_rows: list[dict[str, Any]],
    adjudicated: list[dict[str, Any]],
    args: argparse.Namespace,
) -> None:
    root_agg = aggregate_root_metrics(root_rows)
    cmd = " ".join(shlex.quote(part) for part in sys.argv)
    by_config: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in adjudicated:
        by_config[str(row["config"])].append(row)

    lines: list[str] = []
    lines.append("# Policy v2 Root-Set Stockfish Adjudication")
    lines.append("")
    lines.append(f"- Command: `{cmd}`")
    lines.append(f"- Stockfish: `{args.stockfish}`")
    lines.append(f"- Depth: {args.depth}")
    lines.append(f"- Threads / hash: {args.threads} / {args.hash_mb} MB")
    lines.append(f"- Input root bench: `{args.input}`")
    lines.append("")
    lines.append("## Root Bench Context")
    lines.append("")
    lines.append("| config | positions | changed moves | change rate | node ratio | nps ratio | wall ratio |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|")
    for label, item in root_agg.items():
        positions = int(item["positions"])
        changes = int(item["changes"])
        lines.append(
            f"| {label} | {positions} | {changes} | {pct(ratio(changes, positions))} | "
            f"{float(item['node_ratio']):.4f} | {float(item['nps_ratio']):.4f} | "
            f"{float(item['wall_ratio']):.4f} |"
        )

    lines.append("")
    lines.append("## Stockfish Preference")
    lines.append("")
    lines.append("| config | adjudicated | SF prefers candidate | avg cp gain | avg baseline gap | avg candidate gap |")
    lines.append("|---|---:|---:|---:|---:|---:|")
    for label, cfg_rows in by_config.items():
        gains = [float(row["candidate_cp_gain"]) for row in cfg_rows]
        better = sum(1 for row in cfg_rows if row["candidate_better"])
        base_gaps = [float(row["baseline_gap_cp"]) for row in cfg_rows]
        cand_gaps = [float(row["candidate_gap_cp"]) for row in cfg_rows]
        lines.append(
            f"| {label} | {len(cfg_rows)} | {pct(ratio(better, len(cfg_rows)))} | "
            f"{mean(gains):.2f} | {mean(base_gaps):.2f} | {mean(cand_gaps):.2f} |"
        )

    def split_table(title: str, key: str) -> None:
        lines.append("")
        lines.append(f"## {title}")
        lines.append("")
        lines.append("| config | bucket | count | SF prefers candidate | avg cp gain |")
        lines.append("|---|---|---:|---:|---:|")
        grouped: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
        for row in adjudicated:
            grouped[(str(row["config"]), str(row.get(key, "unknown")))].append(row)
        for (label, bucket), bucket_rows in sorted(grouped.items()):
            better = sum(1 for row in bucket_rows if row["candidate_better"])
            gains = [float(row["candidate_cp_gain"]) for row in bucket_rows]
            lines.append(
                f"| {label} | {bucket} | {len(bucket_rows)} | "
                f"{pct(ratio(better, len(bucket_rows)))} | {mean(gains):.2f} |"
            )

    split_table("Phase Split", "phase")
    split_table("Candidate Move-Class Split", "candidate_move_class")
    split_table("Baseline Policy-Rank Bucket Split", "baseline_rank_bucket")

    lines.append("")
    lines.append("## Output Files")
    lines.append("")
    lines.append(f"- JSONL: `{args.out_jsonl}`")
    lines.append(f"- CSV: `{args.out_csv}`")
    lines.append("")
    lines.append("Notes:")
    lines.append("- Stockfish is used only as a root-move oracle for positions where the candidate changed the engine move.")
    lines.append("- `baseline_rank_bucket` uses the baseline move's rank in the candidate policy ordering; the engine does not expose a full policy-off root ranking through UCI.")

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--stockfish", type=Path, default=Path("stockfish"))
    parser.add_argument("--config-label", action="append",
                        help="Adjudicate only this config label; may be repeated.")
    parser.add_argument("--max-changed-per-config", type=int, default=200)
    parser.add_argument("--depth", type=int, default=10)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--hash-mb", type=int, default=64)
    parser.add_argument("--out-jsonl", type=Path, required=True)
    parser.add_argument("--out-csv", type=Path, required=True)
    parser.add_argument("--summary-md", type=Path, required=True)
    parser.add_argument("--keep-hash", action="store_true")
    args = parser.parse_args()

    stockfish = args.stockfish
    if not stockfish.is_absolute():
        resolved = shutil_which(str(stockfish))
        stockfish = Path(resolved) if resolved else (Path.cwd() / stockfish)
    if not stockfish.exists() or not os.access(stockfish, os.X_OK):
        raise SystemExit(f"stockfish not found/executable: {stockfish}")

    rows = load_jsonl(args.input)
    labels = set(args.config_label) if args.config_label else None
    changed = legal_changed_rows(rows, labels, args.max_changed_per_config)
    if not changed:
        raise SystemExit("no changed legal candidate rows to adjudicate")

    args.out_jsonl.parent.mkdir(parents=True, exist_ok=True)
    args.out_csv.parent.mkdir(parents=True, exist_ok=True)
    adjudicated: list[dict[str, Any]] = []

    sf = UciEngine(stockfish)
    try:
        sf.configure(args.threads, args.hash_mb, multipv=1)
        with args.out_jsonl.open("w", encoding="utf-8") as jsonl, args.out_csv.open(
            "w", newline="", encoding="utf-8"
        ) as csv_file:
            writer = csv.DictWriter(csv_file, fieldnames=CSV_FIELDS)
            writer.writeheader()

            for idx, row in enumerate(changed, start=1):
                fen = str(row["fen"])
                baseline_move = str(row["baseline_bestmove"])
                candidate_move = str(row["bestmove"])
                full = sf.search(fen, args.depth, clear_hash=not args.keep_hash)
                baseline = sf.search(fen, args.depth, baseline_move, clear_hash=not args.keep_hash)
                candidate = sf.search(fen, args.depth, candidate_move, clear_hash=not args.keep_hash)
                best_cp = full["score_as_cp"]
                base_cp = baseline["score_as_cp"]
                cand_cp = candidate["score_as_cp"]
                if best_cp is None or base_cp is None or cand_cp is None:
                    continue
                base_gap = int(best_cp) - int(base_cp)
                cand_gap = int(best_cp) - int(cand_cp)
                gain = int(cand_cp) - int(base_cp)
                out = {
                    "position_id": row["position_id"],
                    "fen": fen,
                    "config": row["config"],
                    "phase": row.get("phase", ""),
                    "candidate_move_class": row.get("move_class", ""),
                    "baseline_move_class": row.get("baseline_move_class", ""),
                    "baseline_move_policy_rank": row.get("baseline_move_policy_rank"),
                    "baseline_rank_bucket": rank_bucket(row.get("baseline_move_policy_rank")),
                    "baseline_move": baseline_move,
                    "candidate_move": candidate_move,
                    "stockfish_bestmove": full["bestmove"],
                    "stockfish_best_cp": int(best_cp),
                    "baseline_cp": int(base_cp),
                    "candidate_cp": int(cand_cp),
                    "baseline_gap_cp": int(base_gap),
                    "candidate_gap_cp": int(cand_gap),
                    "candidate_cp_gain": int(gain),
                    "candidate_better": gain > 0,
                    "sf_depth": args.depth,
                    "full_nodes": full["nodes"],
                    "baseline_nodes": baseline["nodes"],
                    "candidate_nodes": candidate["nodes"],
                    "full_wall_ms": full["wall_ms"],
                    "baseline_wall_ms": baseline["wall_ms"],
                    "candidate_wall_ms": candidate["wall_ms"],
                }
                adjudicated.append(out)
                jsonl.write(json.dumps(out, separators=(",", ":"), ensure_ascii=True) + "\n")
                writer.writerow(out)
                if idx % 25 == 0:
                    print(f"adjudicated changed moves: {idx}/{len(changed)}", flush=True)
    finally:
        sf.close()

    write_summary(args.summary_md, rows, adjudicated, args)
    print(f"wrote {args.out_jsonl}")
    print(f"wrote {args.out_csv}")
    print(f"wrote {args.summary_md}")
    return 0


def shutil_which(name: str) -> str | None:
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        candidate = Path(directory) / name
        if candidate.exists() and os.access(candidate, os.X_OK):
            return str(candidate)
    return None


if __name__ == "__main__":
    raise SystemExit(main())
