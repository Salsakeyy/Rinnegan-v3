#!/usr/bin/env python3
"""Create legal candidate FENs for NNUE labeling.

The generator prefers PGN extraction when PGNs are supplied. Otherwise it starts
from the local opening EPD and performs weighted random legal playouts. These
positions are candidates only; label_with_stockfish.py does the expensive
Stockfish depth-40 scoring pass.
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path
from typing import Iterable, Iterator

try:
    import chess
    import chess.pgn
except ImportError as exc:  # pragma: no cover - dependency guard
    raise SystemExit(
        "Missing dependency: python-chess. Install with "
        "`python3 -m pip install -r tools/nnue/requirements.txt`."
    ) from exc


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SEEDS = ROOT / "tests" / "openings.epd"
START_FEN = chess.STARTING_FEN


def fen_from_epd_line(line: str) -> str | None:
    text = line.strip()
    if not text or text.startswith("#"):
        return None
    fields = text.split()
    if len(fields) < 4:
        return None
    fen = " ".join(fields[:4]) + " 0 1"
    try:
        chess.Board(fen)
    except ValueError:
        return None
    return fen


def load_seed_fens(paths: Iterable[Path]) -> list[str]:
    seeds: list[str] = []
    for path in paths:
        if not path.exists():
            continue
        for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
            fen = fen_from_epd_line(line)
            if fen:
                seeds.append(fen)
    return seeds or [START_FEN]


def fen_key(board: chess.Board) -> str:
    fields = board.fen().split()
    return " ".join(fields[:4])


def keep_position(board: chess.Board, min_pieces: int, max_abs_material_cp: int | None) -> bool:
    if board.is_game_over(claim_draw=True):
        return False
    if len(board.piece_map()) < min_pieces:
        return False
    if max_abs_material_cp is None:
        return True

    values = {
        chess.PAWN: 100,
        chess.KNIGHT: 320,
        chess.BISHOP: 330,
        chess.ROOK: 500,
        chess.QUEEN: 900,
        chess.KING: 0,
    }
    material = 0
    for piece in board.piece_map().values():
        sign = 1 if piece.color == chess.WHITE else -1
        material += sign * values[piece.piece_type]
    return abs(material) <= max_abs_material_cp


def move_weight(board: chess.Board, move: chess.Move) -> float:
    weight = 1.0
    if board.is_capture(move):
        weight += 4.0
    if board.gives_check(move):
        weight += 2.0
    if move.promotion:
        weight += 6.0
    if board.is_castling(move):
        weight += 1.5

    file = chess.square_file(move.to_square)
    rank = chess.square_rank(move.to_square)
    center_distance = abs(file - 3.5) + abs(rank - 3.5)
    weight += max(0.0, 3.5 - center_distance) * 0.35
    return weight


def weighted_random_move(board: chess.Board, rng: random.Random) -> chess.Move:
    moves = list(board.legal_moves)
    weights = [move_weight(board, move) for move in moves]
    return rng.choices(moves, weights=weights, k=1)[0]


def pgn_positions(
    pgn_paths: Iterable[Path],
    min_ply: int,
    max_ply: int,
    sample_every: int,
    min_pieces: int,
    max_abs_material_cp: int | None,
) -> Iterator[str]:
    for path in pgn_paths:
        with path.open(encoding="utf-8", errors="ignore") as handle:
            while True:
                game = chess.pgn.read_game(handle)
                if game is None:
                    break
                board = game.board()
                for ply, move in enumerate(game.mainline_moves(), start=1):
                    board.push(move)
                    if ply > max_ply:
                        break
                    if ply >= min_ply and ply % sample_every == 0:
                        if keep_position(board, min_pieces, max_abs_material_cp):
                            yield board.fen()


def random_playout_positions(
    seed_fens: list[str],
    rng: random.Random,
    min_ply: int,
    max_ply: int,
    sample_every: int,
    min_pieces: int,
    max_abs_material_cp: int | None,
) -> Iterator[str]:
    while True:
        board = chess.Board(rng.choice(seed_fens))
        for ply in range(1, max_ply + 1):
            if board.is_game_over(claim_draw=True):
                break
            board.push(weighted_random_move(board, rng))
            if ply >= min_ply and ply % sample_every == 0:
                if keep_position(board, min_pieces, max_abs_material_cp):
                    yield board.fen()


def write_positions(source: Iterator[str], out: Path, count: int, resume: bool, log_every: int) -> int:
    out.parent.mkdir(parents=True, exist_ok=True)
    seen: set[str] = set()
    written = 0

    if resume and out.exists():
        with out.open(encoding="utf-8", errors="ignore") as handle:
            for line in handle:
                fen = line.strip()
                if fen:
                    seen.add(" ".join(fen.split()[:4]))
                    written += 1
        mode = "a"
    else:
        mode = "w"

    with out.open(mode, encoding="utf-8") as handle:
        for fen in source:
            key = " ".join(fen.split()[:4])
            if key in seen:
                continue
            seen.add(key)
            handle.write(fen + "\n")
            written += 1
            if written % log_every == 0:
                print(f"positions={written}", file=sys.stderr, flush=True)
            if written >= count:
                break
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "data" / "nnue" / "candidates.fens")
    parser.add_argument("--count", type=int, default=200_000)
    parser.add_argument("--seed-epd", type=Path, action="append", default=[DEFAULT_SEEDS])
    parser.add_argument("--pgn", type=Path, action="append", default=[])
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--min-ply", type=int, default=8)
    parser.add_argument("--max-ply", type=int, default=120)
    parser.add_argument("--sample-every", type=int, default=2)
    parser.add_argument("--min-pieces", type=int, default=6)
    parser.add_argument("--max-abs-material-cp", type=int, default=1800)
    parser.add_argument("--no-material-filter", action="store_true")
    parser.add_argument("--resume", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--log-every", type=int, default=10_000)
    args = parser.parse_args()

    max_abs_material_cp = None if args.no_material_filter else args.max_abs_material_cp
    rng = random.Random(args.seed)

    if args.pgn:
        source = pgn_positions(
            args.pgn,
            args.min_ply,
            args.max_ply,
            args.sample_every,
            args.min_pieces,
            max_abs_material_cp,
        )
    else:
        seed_fens = load_seed_fens(args.seed_epd)
        source = random_playout_positions(
            seed_fens,
            rng,
            args.min_ply,
            args.max_ply,
            args.sample_every,
            args.min_pieces,
            max_abs_material_cp,
        )

    written = write_positions(source, args.out, args.count, args.resume, args.log_every)
    print(f"Wrote {written} candidate positions to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
