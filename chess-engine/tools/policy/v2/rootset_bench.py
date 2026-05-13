#!/usr/bin/env python3
"""Root-set bench for v2 move-ordering policies.

This is a pre-SPRT diagnostic: it compares policy-off root searches with one
or more v2 configs over a larger FEN set, and records both engine search
outcomes and sidecar policy-root annotations.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import random
import re
import shlex
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import chess
import numpy as np

HERE = Path(__file__).resolve().parent
POLICY_DIR = HERE.parent
ENGINE_ROOT = HERE.parents[2]
sys.path.insert(0, str(POLICY_DIR))

from common import board_from_fen_or_epd, open_text  # noqa: E402
from pack_policy_dataset import move_features, move_features_new32  # noqa: E402


CSV_FIELDS = [
    "position_id",
    "fen",
    "config",
    "policy_file",
    "mode",
    "scale",
    "quiet_scale",
    "endgame_scale",
    "bonus_clamp",
    "depth",
    "phase",
    "legal_moves",
    "bestmove",
    "move_class",
    "score_cp",
    "score_mate",
    "nodes",
    "nps",
    "wall_ms",
    "policy_calls",
    "feature_ns",
    "forward_ns",
    "saturated",
    "scored_moves",
    "baseline_bestmove",
    "baseline_move_class",
    "baseline_nodes",
    "baseline_nps",
    "baseline_wall_ms",
    "changed",
    "chosen_policy_rank",
    "chosen_raw",
    "chosen_bonus",
    "chosen_order_bonus",
    "baseline_move_policy_rank",
    "baseline_move_policy_bonus",
    "policy_top",
]


@dataclass(frozen=True)
class V2Config:
    label: str
    policy_file: Path
    mode: int
    scale: int
    quiet_scale: int
    endgame_scale: int
    bonus_clamp: int = 20000

    @staticmethod
    def parse(text: str, base_dir: Path) -> "V2Config":
        parts = text.split(":")
        if len(parts) not in (6, 7):
            raise ValueError(
                "--config must be label:policy_file:mode:scale:quiet_scale:endgame_scale[:bonus_clamp]"
            )
        label, policy_file, mode, scale, quiet_scale, endgame_scale = parts[:6]
        bonus_clamp = parts[6] if len(parts) == 7 else "20000"
        path = Path(policy_file)
        if not path.is_absolute():
            path = (base_dir / path).resolve()
        return V2Config(
            label=label,
            policy_file=path,
            mode=int(mode),
            scale=int(scale),
            quiet_scale=int(quiet_scale),
            endgame_scale=int(endgame_scale),
            bonus_clamp=int(bonus_clamp),
        )


@dataclass
class SearchResult:
    bestmove: str
    nodes: int
    nps: int
    wall_ms: float
    score_cp: int | None = None
    score_mate: int | None = None


class UciEngine:
    def __init__(self, engine: Path, cwd: Path):
        self.proc = subprocess.Popen(
            [str(engine)],
            cwd=str(cwd),
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
            return
        if isinstance(value, bool):
            value = "true" if value else "false"
        self._send(f"setoption name {name} value {value}")

    def ready(self) -> None:
        self._send("isready")
        self._read_until("readyok")

    def configure(self, threads: int, hash_mb: int, options: dict[str, Any]) -> None:
        self.setoption("Threads", threads)
        self.setoption("Hash", hash_mb)
        for name, value in options.items():
            self.setoption(name, value)
        self.ready()
        self.policy_stats(reset=True)

    def search(self, fen: str, depth: int, clear_hash: bool = True) -> SearchResult:
        if clear_hash:
            self.setoption("Clear Hash")
        self._send(f"position fen {fen}")
        start = time.perf_counter()
        self._send(f"go depth {depth}")

        bestmove = "0000"
        nodes = 0
        nps = 0
        score_cp: int | None = None
        score_mate: int | None = None

        while True:
            line = self._readline()
            if line.startswith("info "):
                parsed = parse_info_line(line)
                nodes = parsed.get("nodes", nodes)
                nps = parsed.get("nps", nps)
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
        return SearchResult(
            bestmove=bestmove,
            nodes=nodes,
            nps=nps,
            wall_ms=wall_ms,
            score_cp=score_cp,
            score_mate=score_mate,
        )

    def policy_stats(self, reset: bool = True) -> dict[str, int]:
        self._send("policystats reset" if reset else "policystats")
        while True:
            line = self._readline()
            if "policy_calls=" not in line:
                continue
            out: dict[str, int] = {}
            # `saturated` / `scored_moves` were added in the bonus-clamp
            # diagnostic pass. Older engines that pre-date them simply don't
            # emit the keys and the regex falls through to 0.
            for key in ("policy_calls", "feature_ns", "forward_ns",
                        "saturated", "scored_moves"):
                match = re.search(rf"{key}=(-?\d+)", line)
                out[key] = int(match.group(1)) if match else 0
            return out

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
        if token in {"nodes", "nps", "time"} and i + 1 < len(tokens):
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


class PolicyV2Model:
    def __init__(self, path: Path):
        data = path.read_bytes()
        if len(data) < 36:
            raise ValueError(f"{path} is too small to be RINPOL2")
        magic, version, layout_id, feature_dim, h1, h2, buckets, phases = struct.unpack_from(
            "<8s7I", data, 0
        )
        if magic != b"RINPOL2\0" or version != 1:
            raise ValueError(f"{path} is not a RINPOL2 v1 binary")
        self.path = path
        self.layout_id = int(layout_id)
        self.feature_dim = int(feature_dim)
        self.hidden1 = int(h1)
        self.hidden2 = int(h2)
        self.buckets = int(buckets)
        self.phases = int(phases)

        offset = struct.calcsize("<8s7I")

        def take(count: int) -> np.ndarray:
            nonlocal offset
            end = offset + count * 4
            if end > len(data):
                raise ValueError(f"{path} ended while reading model arrays")
            arr = np.frombuffer(data, dtype="<f4", count=count, offset=offset).astype(
                np.float32, copy=True
            )
            offset = end
            return arr

        self.w1 = take(self.hidden1 * self.feature_dim).reshape(self.hidden1, self.feature_dim)
        self.b1 = take(self.hidden1)
        self.w2 = take(self.hidden2 * self.hidden1).reshape(self.hidden2, self.hidden1)
        self.b2 = take(self.hidden2)
        self.w3 = take(self.hidden2)
        self.b3 = float(take(1)[0])
        self.bucket_scale = take(self.buckets)
        self.bucket_bias = take(self.buckets)
        self.phase_scale = take(self.phases) if self.phases else np.ones(0, dtype=np.float32)

    def features(self, board: chess.Board, move: chess.Move) -> list[float]:
        if self.layout_id == 1:
            return move_features_new32(board, move)
        if self.layout_id == 0:
            return move_features(board, move, self.feature_dim)
        raise ValueError("new64 sidecar policy scoring is not implemented")

    def forward(self, x: np.ndarray) -> np.ndarray:
        h1 = np.maximum(x @ self.w1.T + self.b1, 0.0)
        h2 = np.maximum(h1 @ self.w2.T + self.b2, 0.0)
        return h2 @ self.w3 + self.b3


def lround(value: float) -> int:
    if value >= 0.0:
        return int(math.floor(value + 0.5))
    return int(math.ceil(value - 0.5))


def clamp_int(value: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, value))


def phase_bucket(board: chess.Board) -> str:
    ratio = len(board.piece_map()) / 32.0
    if ratio > 0.78:
        return "opening"
    if ratio > 0.42:
        return "middlegame"
    return "endgame"


def phase_index(piece_ratio: float, phases: int) -> int:
    if phases <= 1:
        return 0
    if phases == 2:
        return 0 if piece_ratio > 0.5 else 1
    if piece_ratio > 0.78:
        return 0
    if piece_ratio > 0.42:
        return 1
    return min(phases - 1, 2)


def move_class(board: chess.Board, move_uci: str) -> str:
    if not move_uci or move_uci == "0000":
        return "none"
    try:
        move = chess.Move.from_uci(move_uci)
    except ValueError:
        return "invalid"
    if move not in board.legal_moves:
        return "illegal"
    if move.promotion:
        return "promotion"
    if board.is_capture(move):
        return "capture"
    if board.gives_check(move):
        return "check"
    return "quiet"


def is_engine_quiet(board: chess.Board, move: chess.Move) -> bool:
    return (not board.is_capture(move)) and (not move.promotion)


def bucket_index(board: chess.Board, move: chess.Move) -> int:
    if move.promotion:
        return 3
    if board.is_capture(move):
        return 1
    return 0


def score_policy_moves(
    board: chess.Board,
    config: V2Config,
    model: PolicyV2Model,
) -> list[dict[str, Any]]:
    moves = list(board.legal_moves)
    if not moves:
        return []

    x = np.asarray([model.features(board, move) for move in moves], dtype=np.float32)
    raw_scores = model.forward(x)

    piece_ratio = len(board.piece_map()) / 32.0
    pidx = phase_index(piece_ratio, model.phases)
    phase_mul = float(model.phase_scale[pidx]) if model.phases else 1.0
    if pidx == 2:
        phase_mul *= float(config.endgame_scale) / 100.0

    rows: list[dict[str, Any]] = []
    for move, raw_value in zip(moves, raw_scores):
        bucket = bucket_index(board, move)
        bscale = float(model.bucket_scale[bucket]) if bucket < model.buckets else 1.0
        bbias = float(model.bucket_bias[bucket]) if bucket < model.buckets else 0.0
        quiet = is_engine_quiet(board, move)
        engine_scale = config.quiet_scale if quiet else config.scale
        scaled = (float(raw_value) + bbias) * bscale * phase_mul
        clamp = clamp_int(config.bonus_clamp, 1000, 60000)
        bonus = clamp_int(lround(scaled * float(engine_scale)), -clamp, clamp)
        order_bonus = 0 if config.mode == 2 and not quiet else bonus
        rows.append(
            {
                "move": move.uci(),
                "raw": float(raw_value),
                "bonus": int(bonus),
                "order_bonus": int(order_bonus),
                "bucket": bucket,
                "move_class": move_class(board, move.uci()),
                "quiet_for_ordering": quiet,
            }
        )

    rows.sort(
        key=lambda row: (
            int(row["order_bonus"]),
            int(row["bonus"]),
            float(row["raw"]),
            row["move"],
        ),
        reverse=True,
    )
    for idx, row in enumerate(rows, start=1):
        row["policy_rank"] = idx
    return rows


def raw_fen_from_line(line: str) -> str | None:
    value = line.strip()
    if not value or value.startswith("#"):
        return None
    if value.startswith("{"):
        try:
            record = json.loads(value)
        except json.JSONDecodeError:
            return None
        fen = record.get("fen")
        return str(fen) if fen else None
    return value


def load_root_fens(
    path: Path,
    sample_size: int,
    limit: int,
    seed: int,
    max_input_lines: int,
    dedupe: bool,
) -> list[str]:
    rng = random.Random(seed)
    reservoir: list[str] = []
    seen = 0

    with open_text(path, "rt") as handle:
        for raw in handle:
            if max_input_lines and seen >= max_input_lines:
                break
            fen = raw_fen_from_line(raw)
            if fen is None:
                continue
            seen += 1
            if sample_size > 0:
                if len(reservoir) < sample_size:
                    reservoir.append(fen)
                else:
                    j = rng.randrange(seen)
                    if j < sample_size:
                        reservoir[j] = fen
            else:
                reservoir.append(fen)
                if limit and len(reservoir) >= limit:
                    break

    parsed: list[str] = []
    keys: set[str] = set()
    for raw in reservoir:
        try:
            board = board_from_fen_or_epd(raw)
        except Exception:
            continue
        if board.is_game_over(claim_draw=False):
            continue
        key = " ".join(board.fen().split()[:4])
        if dedupe and key in keys:
            continue
        keys.add(key)
        parsed.append(board.fen())
        if not sample_size and limit and len(parsed) >= limit:
            break
    return parsed


def json_dumps_compact(value: Any) -> str:
    return json.dumps(value, separators=(",", ":"), ensure_ascii=True)


def csv_row(row: dict[str, Any], top_k: int) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for key in CSV_FIELDS:
        if key == "policy_top":
            out[key] = json_dumps_compact(row.get("policy_moves", [])[:top_k])
        else:
            value = row.get(key, "")
            if isinstance(value, bool):
                value = int(value)
            out[key] = value if value is not None else ""
    return out


def policy_lookup(policy_moves: list[dict[str, Any]], move: str) -> dict[str, Any] | None:
    for row in policy_moves:
        if row["move"] == move:
            return row
    return None


def make_row(
    position_id: int,
    fen: str,
    config_label: str,
    result: SearchResult,
    board: chess.Board,
    depth: int,
    policy_file: str = "",
    mode: int | str = "",
    scale: int | str = "",
    quiet_scale: int | str = "",
    endgame_scale: int | str = "",
    bonus_clamp: int | str = "",
    baseline: SearchResult | None = None,
    baseline_class: str = "",
    policy_moves: list[dict[str, Any]] | None = None,
    stats: dict[str, int] | None = None,
) -> dict[str, Any]:
    policy_moves = policy_moves or []
    stats = stats or {}
    chosen = policy_lookup(policy_moves, result.bestmove)
    baseline_policy = policy_lookup(policy_moves, baseline.bestmove) if baseline else None
    return {
        "position_id": position_id,
        "fen": fen,
        "config": config_label,
        "policy_file": policy_file,
        "mode": mode,
        "scale": scale,
        "quiet_scale": quiet_scale,
        "endgame_scale": endgame_scale,
        "bonus_clamp": bonus_clamp,
        "depth": depth,
        "phase": phase_bucket(board),
        "legal_moves": board.legal_moves.count(),
        "bestmove": result.bestmove,
        "move_class": move_class(board, result.bestmove),
        "score_cp": result.score_cp,
        "score_mate": result.score_mate,
        "nodes": result.nodes,
        "nps": result.nps,
        "wall_ms": result.wall_ms,
        "policy_calls": stats.get("policy_calls", 0),
        "feature_ns": stats.get("feature_ns", 0),
        "forward_ns": stats.get("forward_ns", 0),
        "saturated": stats.get("saturated", 0),
        "scored_moves": stats.get("scored_moves", 0),
        "baseline_bestmove": baseline.bestmove if baseline else result.bestmove,
        "baseline_move_class": baseline_class or move_class(board, result.bestmove),
        "baseline_nodes": baseline.nodes if baseline else result.nodes,
        "baseline_nps": baseline.nps if baseline else result.nps,
        "baseline_wall_ms": baseline.wall_ms if baseline else result.wall_ms,
        "changed": bool(baseline and result.bestmove != baseline.bestmove),
        "chosen_policy_rank": chosen.get("policy_rank") if chosen else None,
        "chosen_raw": chosen.get("raw") if chosen else None,
        "chosen_bonus": chosen.get("bonus") if chosen else None,
        "chosen_order_bonus": chosen.get("order_bonus") if chosen else None,
        "baseline_move_policy_rank": baseline_policy.get("policy_rank") if baseline_policy else None,
        "baseline_move_policy_bonus": baseline_policy.get("bonus") if baseline_policy else None,
        "policy_moves": policy_moves,
    }


def ratio(num: float, den: float) -> float:
    return num / den if den else 0.0


def pct(value: float) -> str:
    return f"{value * 100.0:.2f}%"


def aggregate_rows(rows: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    off_rows = {int(row["position_id"]): row for row in rows if row["config"] == "off"}
    labels = sorted({str(row["config"]) for row in rows if row["config"] != "off"})
    out: dict[str, dict[str, Any]] = {}
    for label in labels:
        cfg_rows = [row for row in rows if row["config"] == label]
        ids = [int(row["position_id"]) for row in cfg_rows]
        off_subset = [off_rows[i] for i in ids if i in off_rows]
        cfg_nodes = sum(int(row["nodes"]) for row in cfg_rows)
        off_nodes = sum(int(row["nodes"]) for row in off_subset)
        cfg_wall = sum(float(row["wall_ms"]) for row in cfg_rows)
        off_wall = sum(float(row["wall_ms"]) for row in off_subset)
        cfg_nps_agg = cfg_nodes / (cfg_wall / 1000.0) if cfg_wall else 0.0
        off_nps_agg = off_nodes / (off_wall / 1000.0) if off_wall else 0.0
        out[label] = {
            "positions": len(cfg_rows),
            "changes": sum(1 for row in cfg_rows if row["changed"]),
            "node_ratio": ratio(cfg_nodes, off_nodes),
            "wall_ratio": ratio(cfg_wall, off_wall),
            "nps_ratio": ratio(cfg_nps_agg, off_nps_agg),
            "nodes": cfg_nodes,
            "off_nodes": off_nodes,
            "wall_ms": cfg_wall,
            "off_wall_ms": off_wall,
            "feature_ns": sum(int(row["feature_ns"]) for row in cfg_rows),
            "forward_ns": sum(int(row["forward_ns"]) for row in cfg_rows),
            "saturated": sum(int(row.get("saturated", 0)) for row in cfg_rows),
            "scored_moves": sum(int(row.get("scored_moves", 0)) for row in cfg_rows),
        }
    return out


def write_summary(
    path: Path,
    rows: list[dict[str, Any]],
    args: argparse.Namespace,
    configs: list[V2Config],
) -> None:
    agg = aggregate_rows(rows)
    cmd = " ".join(shlex.quote(part) for part in sys.argv)
    lines: list[str] = []
    lines.append("# Policy v2 Root-Set Bench")
    lines.append("")
    lines.append(f"- Command: `{cmd}`")
    lines.append(f"- Engine: `{args.engine}`")
    lines.append(f"- FEN source: `{args.fens}`")
    lines.append(f"- Positions: {len({row['position_id'] for row in rows})}")
    lines.append(f"- Depth: {args.depth}")
    lines.append(f"- Threads / hash: {args.threads} / {args.hash_mb} MB")
    lines.append("")
    lines.append("## Config Summary")
    lines.append("")
    lines.append("| config | clamp | positions | changes | change rate | node ratio | nps ratio | wall ratio | feature ms | forward ms | saturation |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for cfg in configs:
        item = agg.get(cfg.label, {})
        positions = int(item.get("positions", 0))
        changes = int(item.get("changes", 0))
        saturated = int(item.get("saturated", 0))
        scored = int(item.get("scored_moves", 0))
        sat_rate = ratio(saturated, scored)
        lines.append(
            "| {label} | {clamp} | {positions} | {changes} | {change_rate} | {node:.4f} | {nps:.4f} | {wall:.4f} | {feature:.3f} | {forward:.3f} | {sat} |".format(
                label=cfg.label,
                clamp=cfg.bonus_clamp,
                positions=positions,
                changes=changes,
                change_rate=pct(ratio(changes, positions)),
                node=float(item.get("node_ratio", 0.0)),
                nps=float(item.get("nps_ratio", 0.0)),
                wall=float(item.get("wall_ratio", 0.0)),
                feature=float(item.get("feature_ns", 0)) / 1_000_000.0,
                forward=float(item.get("forward_ns", 0)) / 1_000_000.0,
                sat=pct(sat_rate),
            )
        )

    lines.append("")
    lines.append("## Phase Split")
    lines.append("")
    lines.append("| config | phase | positions | changes | change rate | node ratio | wall ratio |")
    lines.append("|---|---|---:|---:|---:|---:|---:|")
    off_rows = {int(row["position_id"]): row for row in rows if row["config"] == "off"}
    for cfg in configs:
        cfg_rows = [row for row in rows if row["config"] == cfg.label]
        for phase in ("opening", "middlegame", "endgame"):
            phase_rows = [row for row in cfg_rows if row["phase"] == phase]
            if not phase_rows:
                continue
            off_subset = [off_rows[int(row["position_id"])] for row in phase_rows]
            cfg_nodes = sum(int(row["nodes"]) for row in phase_rows)
            off_nodes = sum(int(row["nodes"]) for row in off_subset)
            cfg_wall = sum(float(row["wall_ms"]) for row in phase_rows)
            off_wall = sum(float(row["wall_ms"]) for row in off_subset)
            changes = sum(1 for row in phase_rows if row["changed"])
            lines.append(
                f"| {cfg.label} | {phase} | {len(phase_rows)} | {changes} | "
                f"{pct(ratio(changes, len(phase_rows)))} | {ratio(cfg_nodes, off_nodes):.4f} | "
                f"{ratio(cfg_wall, off_wall):.4f} |"
            )

    lines.append("")
    lines.append("## Candidate Move-Class Split")
    lines.append("")
    lines.append("| config | move class | positions | changes | change rate |")
    lines.append("|---|---|---:|---:|---:|")
    for cfg in configs:
        cfg_rows = [row for row in rows if row["config"] == cfg.label]
        for cls in ("quiet", "capture", "check", "promotion", "none", "illegal"):
            cls_rows = [row for row in cfg_rows if row["move_class"] == cls]
            if not cls_rows:
                continue
            changes = sum(1 for row in cls_rows if row["changed"])
            lines.append(
                f"| {cfg.label} | {cls} | {len(cls_rows)} | {changes} | "
                f"{pct(ratio(changes, len(cls_rows)))} |"
            )

    lines.append("")
    lines.append("## Output Files")
    lines.append("")
    lines.append(f"- JSONL: `{args.out_jsonl}`")
    lines.append(f"- CSV: `{args.out_csv}`")
    lines.append("")
    lines.append("Notes:")
    lines.append("- `policy_moves` in JSONL is the full legal root set sorted by sidecar policy ordering.")
    lines.append("- In quiet-residual mode, `order_bonus` is zero for captures and promotions because that is what the root sorter actually uses.")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, default=Path("build/engine"))
    parser.add_argument("--fens", type=Path, required=True)
    parser.add_argument("--config", action="append", required=True,
                        help="label:policy_file:mode:scale:quiet_scale:endgame_scale[:bonus_clamp]")
    parser.add_argument("--sample-size", type=int, default=500,
                        help="Reservoir sample size. Use 0 with --limit for first-N mode.")
    parser.add_argument("--limit", type=int, default=0,
                        help="First-N usable positions when --sample-size=0.")
    parser.add_argument("--seed", type=int, default=20260511)
    parser.add_argument("--max-input-lines", type=int, default=0,
                        help="Cap source lines read before sampling; 0 reads the whole file.")
    parser.add_argument("--depth", type=int, default=8)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--hash-mb", type=int, default=16)
    parser.add_argument("--top-k", type=int, default=8)
    parser.add_argument("--out-jsonl", type=Path, required=True)
    parser.add_argument("--out-csv", type=Path, required=True)
    parser.add_argument("--summary-md", type=Path, required=True)
    parser.add_argument("--no-dedupe", action="store_true")
    parser.add_argument("--keep-hash", action="store_true",
                        help="Do not clear hash before each root search.")
    args = parser.parse_args()

    base_dir = Path.cwd()
    engine = args.engine if args.engine.is_absolute() else (base_dir / args.engine).resolve()
    if not engine.exists():
        raise SystemExit(f"engine not found: {engine}")
    if not os.access(engine, os.X_OK):
        raise SystemExit(f"engine is not executable: {engine}")

    configs = [V2Config.parse(text, base_dir) for text in args.config]
    for cfg in configs:
        if not cfg.policy_file.exists():
            raise SystemExit(f"policy file not found for {cfg.label}: {cfg.policy_file}")

    fens_path = args.fens if args.fens.is_absolute() else (base_dir / args.fens)
    fens = load_root_fens(
        fens_path,
        sample_size=args.sample_size,
        limit=args.limit,
        seed=args.seed,
        max_input_lines=args.max_input_lines,
        dedupe=not args.no_dedupe,
    )
    if not fens:
        raise SystemExit("no usable root positions")

    models: dict[Path, PolicyV2Model] = {}
    for cfg in configs:
        models.setdefault(cfg.policy_file, PolicyV2Model(cfg.policy_file))

    args.out_jsonl.parent.mkdir(parents=True, exist_ok=True)
    args.out_csv.parent.mkdir(parents=True, exist_ok=True)
    rows: list[dict[str, Any]] = []

    off_engine = UciEngine(engine, base_dir)
    cand_engines: dict[str, UciEngine] = {}
    try:
        off_engine.configure(
            args.threads,
            args.hash_mb,
            {"UsePolicy": False, "UsePolicyV2": False},
        )
        for cfg in configs:
            uci = UciEngine(engine, base_dir)
            uci.configure(
                args.threads,
                args.hash_mb,
                {
                    "UsePolicy": False,
                    "PolicyV2File": cfg.policy_file,
                    "UsePolicyV2": True,
                    "PolicyV2Mode": cfg.mode,
                    "PolicyV2Scale": cfg.scale,
                    "PolicyV2QuietScale": cfg.quiet_scale,
                    "PolicyV2EndgameScale": cfg.endgame_scale,
                    "PolicyV2BonusClamp": cfg.bonus_clamp,
                },
            )
            cand_engines[cfg.label] = uci

        with args.out_jsonl.open("w", encoding="utf-8") as jsonl, args.out_csv.open(
            "w", newline="", encoding="utf-8"
        ) as csv_file:
            writer = csv.DictWriter(csv_file, fieldnames=CSV_FIELDS)
            writer.writeheader()

            for position_id, fen in enumerate(fens, start=1):
                board = chess.Board(fen)
                baseline = off_engine.search(fen, args.depth, clear_hash=not args.keep_hash)
                off_stats = off_engine.policy_stats(reset=True)
                baseline_class = move_class(board, baseline.bestmove)
                off_row = make_row(
                    position_id,
                    fen,
                    "off",
                    baseline,
                    board,
                    args.depth,
                    baseline=baseline,
                    baseline_class=baseline_class,
                    stats=off_stats,
                )
                rows.append(off_row)
                jsonl.write(json_dumps_compact(off_row) + "\n")
                writer.writerow(csv_row(off_row, args.top_k))

                for cfg in configs:
                    model = models[cfg.policy_file]
                    policy_moves = score_policy_moves(board, cfg, model)
                    result = cand_engines[cfg.label].search(
                        fen, args.depth, clear_hash=not args.keep_hash
                    )
                    stats = cand_engines[cfg.label].policy_stats(reset=True)
                    row = make_row(
                        position_id,
                        fen,
                        cfg.label,
                        result,
                        board,
                        args.depth,
                        policy_file=str(cfg.policy_file),
                        mode=cfg.mode,
                        scale=cfg.scale,
                        quiet_scale=cfg.quiet_scale,
                        endgame_scale=cfg.endgame_scale,
                        bonus_clamp=cfg.bonus_clamp,
                        baseline=baseline,
                        baseline_class=baseline_class,
                        policy_moves=policy_moves,
                        stats=stats,
                    )
                    rows.append(row)
                    jsonl.write(json_dumps_compact(row) + "\n")
                    writer.writerow(csv_row(row, args.top_k))

                if position_id % 50 == 0:
                    print(f"bench positions complete: {position_id}/{len(fens)}", flush=True)

    finally:
        off_engine.close()
        for engine_proc in cand_engines.values():
            engine_proc.close()

    write_summary(args.summary_md, rows, args, configs)
    print(f"wrote {args.out_jsonl}")
    print(f"wrote {args.out_csv}")
    print(f"wrote {args.summary_md}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
