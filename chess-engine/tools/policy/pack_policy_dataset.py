#!/usr/bin/env python3
"""Pack teacher PV labels into legal-move policy candidates."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import chess
import numpy as np

from common import board_from_fen_or_epd, iter_jsonl


PIECE_VALUES = {
    chess.PAWN: 100,
    chess.KNIGHT: 320,
    chess.BISHOP: 330,
    chess.ROOK: 500,
    chess.QUEEN: 900,
    chess.KING: 0,
}


def piece_value(piece_type: int | None) -> int:
    if not piece_type:
        return 0
    return PIECE_VALUES.get(piece_type, 0)


def centrality(file: int, rank: int) -> float:
    return (3.5 - max(abs(file - 3.5), abs(rank - 3.5))) / 3.5


def move_gain(board: chess.Board, move: chess.Move) -> int:
    captured = board.piece_at(move.to_square)
    if captured is None and board.is_en_passant(move):
        gain = PIECE_VALUES[chess.PAWN]
    else:
        gain = piece_value(captured.piece_type if captured else 0)
    if move.promotion:
        gain += piece_value(move.promotion) - PIECE_VALUES[chess.PAWN]
    return gain


def best_capture_exchange(board: chess.Board, target: chess.Square, depth: int = 0) -> int:
    if depth >= 32:
        return 0
    best = 0
    for move in board.legal_moves:
        if move.to_square != target:
            continue
        if not board.is_capture(move):
            continue
        gain = move_gain(board, move)
        board.push(move)
        score = gain - best_capture_exchange(board, target, depth + 1)
        board.pop()
        if score > best:
            best = score
    return best


def static_exchange_score(board: chess.Board, move: chess.Move) -> int:
    gain = move_gain(board, move)
    target = move.to_square
    board.push(move)
    score = gain - best_capture_exchange(board, target)
    board.pop()
    return score


def one_hot_piece(piece_type: int, piece_types: list[int]) -> list[float]:
    return [1.0 if piece_type == candidate else 0.0 for candidate in piece_types]


def lowest_attacker_value(board: chess.Board, color: chess.Color, square: chess.Square) -> int:
    values = [
        piece_value(board.piece_type_at(attacker))
        for attacker in board.attackers(color, square)
    ]
    return min(values) if values else 0


def mobility_count(board: chess.Board, square: chess.Square) -> int:
    piece = board.piece_at(square)
    return len(board.attacks(square)) if piece else 0


def attacks_after_move(board: chess.Board, move: chess.Move) -> tuple[bool, bool, int]:
    mover = board.piece_at(move.from_square)
    mover_value = piece_value(move.promotion or (mover.piece_type if mover else 0))
    before_mobility = mobility_count(board, move.from_square)

    board.push(move)
    them = board.turn
    after_mobility = mobility_count(board, move.to_square)
    attacks_more_valuable = False
    attacks_undefended = False

    for target in board.attacks(move.to_square):
        victim = board.piece_at(target)
        if victim is None or victim.color != them:
            continue
        if victim.piece_type == chess.KING:
            continue
        victim_value = piece_value(victim.piece_type)
        if victim_value > mover_value:
            attacks_more_valuable = True
        defenders = board.attackers(them, target)
        if not defenders:
            attacks_undefended = True

    board.pop()
    mobility_delta = max(-1.0, min(1.0, (after_mobility - before_mobility) / 28.0))
    return attacks_more_valuable, attacks_undefended, mobility_delta


def teacher_move_from_record(record: dict) -> str:
    bestmove = str(record.get("bestmove", "")).strip()
    if bestmove and bestmove != "0000":
        return bestmove
    pv = str(record.get("pv", "")).strip()
    return pv.split()[0] if pv else ""


def material_balance_for_side(board: chess.Board) -> float:
    white = 0
    black = 0
    for piece in board.piece_map().values():
        value = PIECE_VALUES[piece.piece_type]
        if piece.color == chess.WHITE:
            white += value
        else:
            black += value
    balance = white - black
    if board.turn == chess.BLACK:
        balance = -balance
    return max(-1.0, min(1.0, balance / 2000.0))


def move_features(board: chess.Board, move: chess.Move, feature_dim: int = 17) -> list[float]:
    mover = board.piece_at(move.from_square)
    captured = board.piece_at(move.to_square)
    is_ep = board.is_en_passant(move)
    if captured is None and is_ep:
        captured_type = chess.PAWN
    else:
        captured_type = captured.piece_type if captured else 0

    from_file = chess.square_file(move.from_square)
    from_rank = chess.square_rank(move.from_square)
    to_file = chess.square_file(move.to_square)
    to_rank = chess.square_rank(move.to_square)
    if board.turn == chess.BLACK:
        from_rank = 7 - from_rank
        to_rank = 7 - to_rank

    gives_check = board.gives_check(move)
    phase = len(board.piece_map()) / 32.0
    material = material_balance_for_side(board)
    mover_type = mover.piece_type if mover else 0
    mover_value = piece_value(mover_type)
    captured_value = piece_value(captured_type)
    see = static_exchange_score(board, move)

    features = [
        from_file / 7.0,
        from_rank / 7.0,
        to_file / 7.0,
        to_rank / 7.0,
        (to_file - from_file) / 7.0,
        (to_rank - from_rank) / 7.0,
        mover_type / 6.0,
        captured_type / 6.0,
        (move.promotion or 0) / 6.0,
        1.0 if board.is_capture(move) else 0.0,
        1.0 if move.promotion else 0.0,
        1.0 if board.is_castling(move) else 0.0,
        1.0 if is_ep else 0.0,
        1.0 if gives_check else 0.0,
        phase,
        material,
        math.log2(max(1, board.fullmove_number)) / 10.0,
    ]
    if feature_dim == 17:
        return features
    if feature_dim != 32:
        raise ValueError(f"unsupported feature dim: {feature_dim}")

    from_centrality = centrality(from_file, from_rank)
    to_centrality = centrality(to_file, to_rank)
    attacked_from = board.is_attacked_by(not board.turn, move.from_square)
    was_in_check = board.is_check()
    board.push(move)
    attacked_to = board.is_attacked_by(board.turn, move.to_square)
    board.pop()

    advancement = 0.0
    if mover_type == chess.PAWN:
        advancement = to_rank / 7.0

    features.extend([
        1.0 if board.turn == chess.WHITE else 0.0,
        from_centrality,
        to_centrality,
        to_centrality - from_centrality,
        mover_value / 900.0,
        captured_value / 900.0,
        max(-2.0, min(2.0, see / 1000.0)),
        1.0 if see >= 0 else 0.0,
        1.0 if see < 0 else 0.0,
        1.0 if mover_type == chess.PAWN and abs(to_rank - from_rank) == 2 else 0.0,
        1.0 if attacked_from else 0.0,
        1.0 if attacked_to else 0.0,
        1.0 if was_in_check else 0.0,
        advancement,
        (abs(to_file - from_file) + abs(to_rank - from_rank)) / 14.0,
    ])
    return features


def move_features_new32(board: chess.Board, move: chess.Move) -> list[float]:
    mover = board.piece_at(move.from_square)
    captured = board.piece_at(move.to_square)
    is_ep = board.is_en_passant(move)
    captured_type = chess.PAWN if captured is None and is_ep else (captured.piece_type if captured else 0)
    mover_type = mover.piece_type if mover else 0

    from_file = chess.square_file(move.from_square)
    from_rank = chess.square_rank(move.from_square)
    to_file = chess.square_file(move.to_square)
    to_rank = chess.square_rank(move.to_square)
    if board.turn == chess.BLACK:
        from_rank = 7 - from_rank
        to_rank = 7 - to_rank

    enemy_king = board.king(not board.turn)
    if enemy_king is None:
        king_distance = 1.0
        in_king_ring = 0.0
    else:
        king_file = chess.square_file(enemy_king)
        king_rank = chess.square_rank(enemy_king)
        if board.turn == chess.BLACK:
            king_rank = 7 - king_rank
        distance = max(abs(to_file - king_file), abs(to_rank - king_rank))
        king_distance = distance / 7.0
        in_king_ring = 1.0 if distance <= 1 else 0.0

    see = static_exchange_score(board, move)
    attacked_from = board.is_attacked_by(not board.turn, move.from_square)
    attacks_more_valuable, attacks_undefended, mobility_delta = attacks_after_move(board, move)

    board.push(move)
    them = board.turn
    us = not board.turn
    enemy_attacker = lowest_attacker_value(board, them, move.to_square)
    own_defender = lowest_attacker_value(board, us, move.to_square)
    board.pop()

    features = [
        from_file / 7.0,
        from_rank / 7.0,
        to_file / 7.0,
        to_rank / 7.0,
        (to_rank - from_rank) / 7.0,
    ]
    features.extend(one_hot_piece(mover_type, [
        chess.PAWN, chess.KNIGHT, chess.BISHOP, chess.ROOK, chess.QUEEN, chess.KING
    ]))
    features.extend(one_hot_piece(captured_type, [
        chess.PAWN, chess.KNIGHT, chess.BISHOP, chess.ROOK, chess.QUEEN
    ]))
    features.extend([
        (move.promotion or 0) / 6.0,
        1.0 if board.is_capture(move) else 0.0,
        1.0 if move.promotion else 0.0,
        1.0 if board.is_castling(move) else 0.0,
        1.0 if board.gives_check(move) else 0.0,
        material_balance_for_side(board),
        len(board.piece_map()) / 32.0,
        max(-2.0, min(2.0, see / 1000.0)),
        1.0 if attacked_from else 0.0,
        enemy_attacker / 900.0,
        own_defender / 900.0,
        king_distance,
        in_king_ring,
        1.0 if attacks_more_valuable else 0.0,
        1.0 if attacks_undefended else 0.0,
        mobility_delta,
    ])
    assert len(features) == 32
    return features


def is_quiet_policy_move(board: chess.Board, move: chess.Move) -> bool:
    return not board.is_capture(move) and not move.promotion


def bucket_flags(board: chess.Board, move: chess.Move) -> int:
    quiet = not board.is_capture(move) and not move.promotion
    flags = 0
    if quiet:
        flags |= 1
    if board.is_capture(move):
        flags |= 2
    if board.gives_check(move):
        flags |= 4
    if move.promotion:
        flags |= 8
    return flags


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--quiet-only", action=argparse.BooleanOptionalAction, default=False,
                        help="Keep only quiet non-promotion move candidates and skip positions whose teacher move is not quiet.")
    parser.add_argument("--feature-dim", type=int, default=17, choices=[17, 32])
    parser.add_argument("--feature-layout", choices=["legacy", "new32"], default="legacy",
                        help="Feature layout to pack. 'legacy' uses --feature-dim; 'new32' packs the proposed 32-feature layout.")
    parser.add_argument("--compressed", action=argparse.BooleanOptionalAction, default=True)
    args = parser.parse_args()

    if args.feature_layout == "new32":
        args.feature_dim = 32

    features: list[list[float]] = []
    labels: list[float] = []
    offsets = [0]
    target_indices: list[int] = []
    bucket_flags_by_group: list[int] = []
    fens: list[str] = []
    bestmoves: list[str] = []
    skipped = 0

    for record in iter_jsonl(args.input):
        if args.limit > 0 and len(fens) >= args.limit:
            break

        fen = record.get("fen")
        teacher_uci = teacher_move_from_record(record)
        if not fen or not teacher_uci:
            skipped += 1
            continue

        try:
            board = board_from_fen_or_epd(str(fen))
            teacher_move = chess.Move.from_uci(teacher_uci)
        except Exception:
            skipped += 1
            continue

        legal_moves = list(board.legal_moves)
        if teacher_move not in legal_moves:
            skipped += 1
            continue
        if args.quiet_only:
            if not is_quiet_policy_move(board, teacher_move):
                skipped += 1
                continue
            legal_moves = [move for move in legal_moves if is_quiet_policy_move(board, move)]
            if len(legal_moves) < 2:
                skipped += 1
                continue

        start = len(features)
        target = -1
        for idx, move in enumerate(legal_moves):
            if args.feature_layout == "new32":
                features.append(move_features_new32(board, move))
            else:
                features.append(move_features(board, move, args.feature_dim))
            is_target = move == teacher_move
            labels.append(1.0 if is_target else 0.0)
            if is_target:
                target = idx

        if target < 0:
            del features[start:]
            del labels[start:]
            skipped += 1
            continue

        offsets.append(len(features))
        target_indices.append(target)
        bucket_flags_by_group.append(bucket_flags(board, legal_moves[target]))
        fens.append(board.fen())
        bestmoves.append(teacher_move.uci())

    if not fens:
        raise RuntimeError("no usable policy positions")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    arrays = {
        "features": np.asarray(features, dtype=np.float32),
        "labels": np.asarray(labels, dtype=np.float32),
        "group_offsets": np.asarray(offsets, dtype=np.int64),
        "target_indices": np.asarray(target_indices, dtype=np.int64),
        "bucket_flags": np.asarray(bucket_flags_by_group, dtype=np.int64),
        "meta": np.asarray(json.dumps({
            "source": str(args.input),
            "positions": len(fens),
            "candidates": len(features),
            "feature_dim": len(features[0]),
            "requested_feature_dim": args.feature_dim,
            "feature_layout": args.feature_layout,
            "quiet_only": args.quiet_only,
            "skipped": skipped,
        })),
    }
    if args.compressed:
        np.savez_compressed(args.output, **arrays)
    else:
        np.savez(args.output, **arrays)

    with args.output.with_suffix(args.output.suffix + ".fens.txt").open("w", encoding="utf-8") as handle:
        for fen in fens:
            handle.write(fen + "\n")
    with args.output.with_suffix(args.output.suffix + ".bestmoves.txt").open("w", encoding="utf-8") as handle:
        for bestmove in bestmoves:
            handle.write(bestmove + "\n")

    print(f"packed {len(fens)} positions, {len(features)} move candidates to {args.output}; skipped {skipped}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
