#!/usr/bin/env python3
"""Shared helpers for policy data tooling."""

from __future__ import annotations

import gzip
import json
from pathlib import Path
from typing import Iterable, Iterator, TextIO

import chess


def open_text(path: str | Path, mode: str = "rt") -> TextIO:
    path = Path(path)
    if path.suffix == ".gz":
        return gzip.open(path, mode, encoding="utf-8")
    return path.open(mode, encoding="utf-8")


def board_from_fen_or_epd(text: str) -> chess.Board:
    value = text.strip()
    if not value:
        raise ValueError("empty FEN/EPD")

    try:
        return chess.Board(value)
    except ValueError:
        board = chess.Board()
        board.set_epd(value)
        return board


def fen_key(board: chess.Board) -> str:
    """Key without move clocks, suitable for duplicate filtering."""
    return " ".join(board.fen().split()[:4])


def iter_fens(path: str | Path) -> Iterator[str]:
    with open_text(path, "rt") as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("{"):
                record = json.loads(line)
                fen = record.get("fen")
                if fen:
                    yield fen
                continue
            yield board_from_fen_or_epd(line).fen()


def iter_jsonl(path: str | Path) -> Iterator[dict]:
    with open_text(path, "rt") as handle:
        for raw in handle:
            line = raw.strip()
            if line:
                yield json.loads(line)


def write_jsonl(path: str | Path, records: Iterable[dict]) -> int:
    count = 0
    with open_text(path, "wt") as handle:
        for record in records:
            handle.write(json.dumps(record, separators=(",", ":")) + "\n")
            count += 1
    return count
