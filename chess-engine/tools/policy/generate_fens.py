#!/usr/bin/env python3
"""Generate diverse legal FENs for policy labeling."""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

import chess

from common import board_from_fen_or_epd, fen_key


def parse_ply_buckets(spec: str) -> list[tuple[int, int, float]]:
    buckets: list[tuple[int, int, float]] = []
    if not spec:
        return buckets

    for raw_bucket in spec.split(","):
        bucket = raw_bucket.strip()
        if not bucket:
            continue
        span, weight_text = bucket.split(":", 1)
        lo_text, hi_text = span.split("-", 1)
        lo = int(lo_text)
        hi = int(hi_text)
        weight = float(weight_text)
        if lo < 0 or hi < lo or weight <= 0.0:
            raise ValueError(f"invalid ply bucket: {raw_bucket!r}")
        buckets.append((lo, hi, weight))
    return buckets


def choose_target_ply(args: argparse.Namespace, rng: random.Random,
                      buckets: list[tuple[int, int, float]]) -> int:
    if not buckets:
        return rng.randint(args.min_ply, args.max_ply)

    total = sum(weight for _, _, weight in buckets)
    pick = rng.random() * total
    acc = 0.0
    for lo, hi, weight in buckets:
        acc += weight
        if pick <= acc:
            return rng.randint(lo, hi)
    lo, hi, _ = buckets[-1]
    return rng.randint(lo, hi)


def load_openings(path: Path | None) -> list[chess.Board]:
    if path is None:
        return [chess.Board()]

    boards: list[chess.Board] = []
    with path.open("r", encoding="utf-8") as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            try:
                boards.append(board_from_fen_or_epd(line))
            except ValueError:
                # EPD files often carry operations after the first four fields.
                fields = line.split()
                if len(fields) >= 4:
                    boards.append(board_from_fen_or_epd(" ".join(fields[:4])))
    return boards or [chess.Board()]


def choose_move(board: chess.Board, rng: random.Random, tactical_rate: float) -> chess.Move:
    moves = list(board.legal_moves)
    tactical: list[chess.Move] = []
    if rng.random() < tactical_rate:
        for move in moves:
            if board.is_capture(move) or board.gives_check(move) or move.promotion:
                tactical.append(move)
    return rng.choice(tactical or moves)


def generate(args: argparse.Namespace) -> list[str]:
    rng = random.Random(args.seed)
    openings = load_openings(args.openings)
    ply_buckets = parse_ply_buckets(args.ply_buckets)
    out: list[str] = []
    seen: set[str] = set()
    attempts = 0
    max_attempts = max(args.count * 200, 1000)

    if args.include_openings:
        for board in openings:
            if len(out) >= args.count:
                break
            if board.is_game_over(claim_draw=True):
                continue
            key = fen_key(board)
            if args.unique and key in seen:
                continue
            seen.add(key)
            out.append(board.fen())

    while len(out) < args.count and attempts < max_attempts:
        attempts += 1
        board = rng.choice(openings).copy(stack=False)
        target_ply = choose_target_ply(args, rng, ply_buckets)

        while board.ply() < target_ply and not board.is_game_over(claim_draw=True):
            board.push(choose_move(board, rng, args.tactical_rate))

        if board.is_game_over(claim_draw=True):
            continue
        if not args.allow_checks and board.is_check():
            continue
        if not args.allow_late_endgames and len(board.piece_map()) <= 5:
            continue

        key = fen_key(board)
        if args.unique and key in seen:
            continue
        seen.add(key)
        out.append(board.fen())

        if args.progress and len(out) % args.progress == 0:
            print(f"generated {len(out)}/{args.count}", file=sys.stderr)

    if len(out) < args.count:
        raise RuntimeError(f"generated only {len(out)} positions after {attempts} attempts")

    return out


def generate_streaming(args: argparse.Namespace) -> int:
    rng = random.Random(args.seed)
    openings = load_openings(args.openings)
    ply_buckets = parse_ply_buckets(args.ply_buckets)
    seen: set[str] = set()
    written = 0

    if args.resume_output and args.output.exists():
        with args.output.open("r", encoding="utf-8") as handle:
            for raw in handle:
                fen = raw.strip()
                if not fen:
                    continue
                try:
                    board = board_from_fen_or_epd(fen)
                except ValueError:
                    continue
                seen.add(fen_key(board))
                written += 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    mode = "a" if written > 0 and args.resume_output else "w"
    attempts = 0
    max_attempts = max(args.count * 300, 1000)

    with args.output.open(mode, encoding="utf-8") as handle:
        if args.include_openings and written == 0:
            for board in openings:
                if written >= args.count:
                    break
                if board.is_game_over(claim_draw=True):
                    continue
                key = fen_key(board)
                if args.unique and key in seen:
                    continue
                seen.add(key)
                handle.write(board.fen() + "\n")
                written += 1

        while written < args.count and attempts < max_attempts:
            attempts += 1
            board = rng.choice(openings).copy(stack=False)
            target_ply = choose_target_ply(args, rng, ply_buckets)

            while board.ply() < target_ply and not board.is_game_over(claim_draw=True):
                board.push(choose_move(board, rng, args.tactical_rate))

            if board.is_game_over(claim_draw=True):
                continue
            if not args.allow_checks and board.is_check():
                continue
            if not args.allow_late_endgames and len(board.piece_map()) <= 5:
                continue

            key = fen_key(board)
            if args.unique and key in seen:
                continue
            seen.add(key)
            handle.write(board.fen() + "\n")
            written += 1

            if args.flush_every > 0 and written % args.flush_every == 0:
                handle.flush()
            if args.progress and written % args.progress == 0:
                print(f"generated {written}/{args.count}", file=sys.stderr)

    if written < args.count:
        raise RuntimeError(f"generated only {written} positions after {attempts} attempts")
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--count", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--openings", type=Path, default=None)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--min-ply", type=int, default=8)
    parser.add_argument("--max-ply", type=int, default=120)
    parser.add_argument(
        "--ply-buckets",
        default="",
        help="Weighted target ply buckets, e.g. '8-16:0.35,17-40:0.40,41-80:0.20,81-120:0.05'",
    )
    parser.add_argument("--include-openings", action="store_true")
    parser.add_argument("--tactical-rate", type=float, default=0.20)
    parser.add_argument("--allow-checks", action="store_true")
    parser.add_argument("--allow-late-endgames", action="store_true")
    parser.add_argument("--unique", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--progress", type=int, default=10000)
    parser.add_argument("--stream-output", action=argparse.BooleanOptionalAction, default=False,
                        help="Write FENs incrementally instead of holding the full list in memory.")
    parser.add_argument("--resume-output", action=argparse.BooleanOptionalAction, default=False,
                        help="Append until --count when --output already contains FENs.")
    parser.add_argument("--flush-every", type=int, default=1000)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.stream_output:
        count = generate_streaming(args)
        print(f"wrote {count} FENs to {args.output}")
        return 0

    fens = generate(args)
    with args.output.open("w", encoding="utf-8") as handle:
        for fen in fens:
            handle.write(fen + "\n")
    print(f"wrote {len(fens)} FENs to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
