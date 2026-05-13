#!/usr/bin/env python3
"""Pack labeled FENs into a v2 policy dataset.

Input is a JSONL file (one record per FEN). The labeler can produce
either a single bestmove per FEN (legacy / hard-bestmove fallback) or a
list of {move, score} entries (Stockfish MultiPV / Rinnegan deep
MultiPV). Both shapes are accepted; missing per-move scores are written
as NaN so the trainer can use grouped CE on the bestmove fallback.

Schema is documented in tools/policy/v2/schema.py. The packer supports
two proposal modes:

  --proposal-mode all_legal     emit one row per legal move
  --proposal-mode proposal_set  emit a curated subset:
                                  * all tactical moves (capture, promo, check)
                                  * top-N quiets ranked by a cheap baseline
                                  * K random quiet negatives
                                  * (always include the teacher bestmove)

The cheap baseline is MVV-LVA + a small SEE penalty for losing captures
plus a centrality + advancement term for quiets. It is a stand-in for
the engine's actual ordering score; the goal is just to give the trainer
something to rank against.
"""

from __future__ import annotations

import argparse
import json
import math
import random
import sys
from pathlib import Path

import chess
import numpy as np

# Make the v1 helpers importable when running as a script.
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from common import board_from_fen_or_epd, iter_jsonl  # noqa: E402
from pack_policy_dataset import move_features_new32  # noqa: E402  (engine-compat path)

from schema import (  # noqa: E402
    SCHEMA_VERSION,
    bucket_flags_for,
    primary_bucket,
)

PIECE_VALUES = {
    chess.PAWN: 100,
    chess.KNIGHT: 320,
    chess.BISHOP: 330,
    chess.ROOK: 500,
    chess.QUEEN: 900,
    chess.KING: 0,
}


def piece_value(pt: int | None) -> int:
    return PIECE_VALUES.get(pt or 0, 0)


def centrality(file: int, rank: int) -> float:
    return (3.5 - max(abs(file - 3.5), abs(rank - 3.5))) / 3.5


def material_balance(board: chess.Board) -> float:
    w = 0
    b = 0
    for piece in board.piece_map().values():
        v = PIECE_VALUES[piece.piece_type]
        if piece.color == chess.WHITE: w += v
        else:                          b += v
    bal = w - b
    if board.turn == chess.BLACK:
        bal = -bal
    return max(-1.0, min(1.0, bal / 2000.0))


# ---------- Feature spaces ----------
#
# The packer supports two feature shapes:
#
#   "split"          F_move=18, F_ctx=6. Compact, CPU-cheap, and avoids
#                    make/unmake. Useful for offline R&D and the trunk
#                    architecture in train_v2.py — but NOT engine-loadable
#                    because the engine extracts a different feature set
#                    at runtime.
#   "new32_compat"   F_move=32 (matches src/policy.cpp::fillNew32FeaturesWithCtx),
#                    F_ctx=0. Used when you intend to export a RINPOL2 binary
#                    and have the engine score moves with the same features
#                    you trained on.
#
# Pick `new32_compat` if your goal is to deploy. Pick `split` if your goal
# is to study fused metrics or validate the trunk architecture.

F_MOVE_SPLIT = 18
F_CTX_SPLIT  = 6
F_MOVE_NEW32 = 32
F_CTX_NEW32  = 0
F_MOVE = F_MOVE_SPLIT

def move_local_features_split(board: chess.Board, move: chess.Move) -> list[float]:
    mover = board.piece_at(move.from_square)
    captured = board.piece_at(move.to_square)
    is_ep = board.is_en_passant(move)
    captured_type = chess.PAWN if (captured is None and is_ep) else (captured.piece_type if captured else 0)

    from_file = chess.square_file(move.from_square)
    from_rank = chess.square_rank(move.from_square)
    to_file   = chess.square_file(move.to_square)
    to_rank   = chess.square_rank(move.to_square)
    if board.turn == chess.BLACK:
        from_rank = 7 - from_rank
        to_rank   = 7 - to_rank

    mover_type = mover.piece_type if mover else 0
    is_capture = board.is_capture(move)
    is_promo   = bool(move.promotion)
    is_castle  = board.is_castling(move)

    feats = [
        from_file / 7.0,
        from_rank / 7.0,
        to_file / 7.0,
        to_rank / 7.0,
        (to_file - from_file) / 7.0,
        (to_rank - from_rank) / 7.0,
        mover_type / 6.0,
        captured_type / 6.0,
        (move.promotion or 0) / 6.0,
        1.0 if is_capture else 0.0,
        1.0 if is_promo   else 0.0,
        1.0 if is_castle  else 0.0,
        1.0 if is_ep      else 0.0,
        # Cheap ordering helpers: piece values, centralities, advancement.
        piece_value(mover_type)    / 900.0,
        piece_value(captured_type) / 900.0,
        centrality(from_file, from_rank),
        centrality(to_file, to_rank),
        (to_rank / 7.0) if mover_type == chess.PAWN else 0.0,
    ]
    assert len(feats) == F_MOVE_SPLIT, f"F_MOVE mismatch: {len(feats)}"
    return feats


def move_local_features_new32(board: chess.Board, move: chess.Move) -> list[float]:
    """Engine-compatible 32-float move features (no separate ctx)."""
    return list(move_features_new32(board, move))


# ---------- Board-context features ----------

F_CTX = F_CTX_SPLIT

def ctx_features_split(board: chess.Board) -> list[float]:
    pieces = len(board.piece_map())
    return [
        pieces / 32.0,
        material_balance(board),
        math.log2(max(1, board.fullmove_number)) / 10.0,
        1.0 if board.turn == chess.WHITE else 0.0,
        1.0 if board.is_check() else 0.0,
        # Has-castling-rights as a coarse opening / castled signal.
        float(any([
            board.has_kingside_castling_rights(chess.WHITE),
            board.has_queenside_castling_rights(chess.WHITE),
            board.has_kingside_castling_rights(chess.BLACK),
            board.has_queenside_castling_rights(chess.BLACK),
        ])),
    ]


# ---------- Cheap baseline ordering score ----------

def baseline_score(board: chess.Board, move: chess.Move) -> float:
    """Stand-in for the engine's classical ordering tier.

    Numerical scale roughly matches `scoreMove` in src/search.cpp:
        capture  : 100000 + MVV-LVA (10..600)
        promotion:  90000 + promo piece type
        quiet    : centrality / advancement / mobility heuristic in [0, 1000]

    The trainer treats this as an opaque rank input. The exact magnitude
    doesn't matter as long as it is monotonic in baseline preference.
    """
    if board.is_capture(move):
        captured_type = chess.PAWN if board.is_en_passant(move) else (board.piece_at(move.to_square).piece_type if board.piece_at(move.to_square) else 0)
        mover = board.piece_at(move.from_square)
        # MVV-LVA: 8 * victim_value - attacker_value-ish
        v = piece_value(captured_type)
        a = piece_value(mover.piece_type if mover else 0)
        return 100000.0 + 8.0 * v - a
    if move.promotion:
        return 90000.0 + (move.promotion or 0)
    # Quiet: centrality of dest + pawn advancement, scaled to ~1000.
    to_file = chess.square_file(move.to_square)
    to_rank = chess.square_rank(move.to_square)
    if board.turn == chess.BLACK:
        to_rank = 7 - to_rank
    mover = board.piece_at(move.from_square)
    pawn_adv = (to_rank / 7.0) if mover and mover.piece_type == chess.PAWN else 0.0
    return 200.0 + 600.0 * centrality(to_file, to_rank) + 200.0 * pawn_adv


# ---------- Teacher record decoding ----------

def teacher_per_move(record: dict) -> dict[str, float]:
    """Return {uci: cp_score} for whatever the labeler produced.

    Recognised shapes:
      * record["multipv"] = [{"move": "...", "cp": 12, "mate": null}, ...]
      * record["per_move"] = [{"move": "...", "score": 12}, ...]
    Otherwise empty.
    """
    out: dict[str, float] = {}
    multipv = record.get("multipv") or record.get("per_move")
    if isinstance(multipv, list):
        for entry in multipv:
            uci = str(entry.get("move", "")).strip()
            if not uci:
                continue
            cp = entry.get("cp")
            if cp is None:
                cp = entry.get("score")
            mate = entry.get("mate")
            if mate is not None:
                cp = (1 if mate > 0 else -1) * (30000 - min(abs(int(mate)), 200))
            if cp is None:
                continue
            out[uci] = float(cp)
    return out


def teacher_bestmove(record: dict) -> str:
    bestmove = str(record.get("bestmove", "")).strip()
    if bestmove and bestmove != "0000":
        return bestmove
    pv = str(record.get("pv", "")).strip()
    return pv.split()[0] if pv else ""


# ---------- Proposal-set selection ----------

def is_quiet(board: chess.Board, move: chess.Move) -> bool:
    return not board.is_capture(move) and not move.promotion


def pick_proposal_set(
    board: chess.Board,
    legal: list[chess.Move],
    teacher_uci: str,
    rng: random.Random,
    top_quiet: int,
    random_quiet: int,
) -> list[chess.Move]:
    """Compose the proposal set — all tactical + top-N + random-K quiets,
    plus the teacher move if it would otherwise be excluded."""
    chosen: list[chess.Move] = []
    chosen_set: set[chess.Move] = set()

    def add(m: chess.Move) -> None:
        if m not in chosen_set:
            chosen.append(m)
            chosen_set.add(m)

    quiets = []
    for m in legal:
        if not is_quiet(board, m):
            add(m)
        else:
            quiets.append(m)

    quiets_scored = sorted(quiets, key=lambda m: -baseline_score(board, m))
    for m in quiets_scored[:max(0, top_quiet)]:
        add(m)

    pool = [m for m in quiets if m not in chosen_set]
    rng.shuffle(pool)
    for m in pool[:max(0, random_quiet)]:
        add(m)

    if teacher_uci:
        try:
            tm = chess.Move.from_uci(teacher_uci)
            if tm in legal:
                add(tm)
        except ValueError:
            pass

    return chosen


# ---------- Packer ----------

def pack(args) -> int:
    rng = random.Random(args.seed)

    # Resolve feature space.
    if args.feature_space == "split":
        f_move_dim = F_MOVE_SPLIT
        f_ctx_dim  = F_CTX_SPLIT
        move_fn = move_local_features_split
        ctx_fn  = ctx_features_split
    elif args.feature_space == "new32_compat":
        f_move_dim = F_MOVE_NEW32
        f_ctx_dim  = F_CTX_NEW32
        move_fn = move_local_features_new32
        ctx_fn  = lambda _board: []  # ctx folded into move features
    else:
        raise ValueError(f"unknown feature space: {args.feature_space}")

    move_feats: list[list[float]] = []
    ctx_feats:  list[list[float]] = []
    baselines:  list[float] = []
    move_buckets: list[int] = []
    teacher_scores: list[float] = []
    proposal_mask: list[int] = []
    offsets: list[int] = [0]
    target_indices: list[int] = []
    bucket_flags_grp: list[int] = []
    fens: list[str] = []
    bestmoves: list[str] = []
    skipped = 0

    teacher_mode_seen: dict[str, int] = {}

    for record in iter_jsonl(args.input):
        if args.limit > 0 and len(fens) >= args.limit:
            break

        fen = record.get("fen")
        teacher_uci = teacher_bestmove(record)
        if not fen or not teacher_uci:
            skipped += 1
            continue

        try:
            board = board_from_fen_or_epd(str(fen))
            teacher_move = chess.Move.from_uci(teacher_uci)
        except Exception:
            skipped += 1
            continue

        legal = list(board.legal_moves)
        if teacher_move not in legal:
            skipped += 1
            continue

        # Decide candidate set.
        if args.proposal_mode == "all_legal":
            cands = legal
        else:
            cands = pick_proposal_set(
                board, legal, teacher_uci, rng,
                top_quiet=args.proposal_top_quiet,
                random_quiet=args.proposal_random_quiet,
            )

        if len(cands) < 2:
            skipped += 1
            continue

        per_move_teacher = teacher_per_move(record)

        # Build rows.
        target = -1
        for idx, m in enumerate(cands):
            move_feats.append(move_fn(board, m))
            cap = board.is_capture(m)
            promo = bool(m.promotion)
            quiet = (not cap) and (not promo)
            gives_check = board.gives_check(m)
            flags = bucket_flags_for(quiet, cap, gives_check, promo)
            move_buckets.append(flags)
            baselines.append(baseline_score(board, m))
            uci = m.uci()
            ts = per_move_teacher.get(uci, math.nan)
            teacher_scores.append(ts)
            proposal_mask.append(1 if args.proposal_mode == "proposal_set" else 0)
            if m == teacher_move:
                target = idx

        if target < 0:
            # Teacher move not in the candidate set — shouldn't happen but
            # bail rather than silently mislabel.
            del move_feats[offsets[-1]:]
            del move_buckets[offsets[-1]:]
            del baselines[offsets[-1]:]
            del teacher_scores[offsets[-1]:]
            del proposal_mask[offsets[-1]:]
            skipped += 1
            continue

        ctx_feats.append(ctx_fn(board))
        offsets.append(len(move_feats))
        target_indices.append(target)
        bucket_flags_grp.append(int(move_buckets[offsets[-2] + target]))
        fens.append(board.fen())
        bestmoves.append(teacher_uci)

        teacher_mode = str(record.get("teacher_mode") or record.get("teacher") or "unknown")
        teacher_mode_seen[teacher_mode] = teacher_mode_seen.get(teacher_mode, 0) + 1

    if not fens:
        raise RuntimeError("no usable policy positions")

    args.output.parent.mkdir(parents=True, exist_ok=True)

    # ctx_feats may be ragged-empty (new32_compat); coerce to a uniform 2-D array.
    if ctx_feats and len(ctx_feats[0]) == 0:
        ctx_arr = np.zeros((len(ctx_feats), 0), dtype=np.float32)
    else:
        ctx_arr = np.asarray(ctx_feats, dtype=np.float32)

    arrays = {
        "schema_version": np.asarray(SCHEMA_VERSION, dtype=np.int32),
        "group_offsets":  np.asarray(offsets, dtype=np.int64),
        "move_features":  np.asarray(move_feats, dtype=np.float32),
        "ctx_features":   ctx_arr,
        "baseline_scores": np.asarray(baselines, dtype=np.float32),
        "move_buckets":   np.asarray(move_buckets, dtype=np.uint8),
        "teacher_scores": np.asarray(teacher_scores, dtype=np.float32),
        "proposal_mask":  np.asarray(proposal_mask, dtype=np.uint8),
        "target_indices": np.asarray(target_indices, dtype=np.int64),
        "bucket_flags":   np.asarray(bucket_flags_grp, dtype=np.uint8),
        "meta": np.asarray(json.dumps({
            "schema_version": SCHEMA_VERSION,
            "source": str(args.input),
            "positions": len(fens),
            "candidates": len(move_feats),
            "F_move": f_move_dim,
            "F_ctx":  f_ctx_dim,
            "feature_space": args.feature_space,
            "proposal_mode": args.proposal_mode,
            "proposal_top_quiet": args.proposal_top_quiet,
            "proposal_random_quiet": args.proposal_random_quiet,
            "teacher_mode_seen": teacher_mode_seen,
            "skipped": skipped,
            "seed": args.seed,
        })),
    }

    if args.compressed:
        np.savez_compressed(args.output, **arrays)
    else:
        np.savez(args.output, **arrays)

    sidecar = args.output.with_suffix(args.output.suffix + ".fens.txt")
    with sidecar.open("w", encoding="utf-8") as h:
        for fen in fens:
            h.write(fen + "\n")
    sidecar = args.output.with_suffix(args.output.suffix + ".bestmoves.txt")
    with sidecar.open("w", encoding="utf-8") as h:
        for bm in bestmoves:
            h.write(bm + "\n")

    has_soft = sum(1 for s in teacher_scores if not math.isnan(s))
    print(
        f"packed {len(fens)} positions, {len(move_feats)} candidates "
        f"({has_soft} with per-move teacher score) to {args.output}; "
        f"skipped {skipped}; mode={args.proposal_mode}"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input",  type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--feature-space", choices=["split", "new32_compat"], default="split",
                        help="'split' = compact F_move=18 / F_ctx=6 (R&D only). "
                             "'new32_compat' = engine-loadable 32-float layout (deploy path).")
    parser.add_argument("--proposal-mode", choices=["all_legal", "proposal_set"], default="all_legal")
    parser.add_argument("--proposal-top-quiet", type=int, default=8)
    parser.add_argument("--proposal-random-quiet", type=int, default=4)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--seed",  type=int, default=0xBEEF)
    parser.add_argument("--compressed", action=argparse.BooleanOptionalAction, default=True)
    args = parser.parse_args()
    return pack(args)


if __name__ == "__main__":
    raise SystemExit(main())
