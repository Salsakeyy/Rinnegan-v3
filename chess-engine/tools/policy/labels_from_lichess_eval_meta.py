#!/usr/bin/env python3
"""Convert extracted Lichess eval metadata into policy teacher labels."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def first_move(line: str) -> str:
    return line.strip().split()[0] if line.strip() else ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    written = 0
    skipped = 0

    with args.input.open("r", encoding="utf-8") as src, args.output.open("w", encoding="utf-8") as dst:
        for raw in src:
            if args.limit > 0 and written >= args.limit:
                break
            try:
                record = json.loads(raw)
            except json.JSONDecodeError:
                skipped += 1
                continue

            fen = str(record.get("fen", "")).strip()
            bestmove = first_move(str(record.get("line", "")))
            if not fen or not bestmove or bestmove == "0000":
                skipped += 1
                continue

            dst.write(json.dumps({
                "fen": fen,
                "depth": record.get("depth"),
                "score": record.get("cp"),
                "mate": record.get("mate"),
                "pv": str(record.get("line", "")),
                "bestmove": bestmove,
                "teacher": "lichess-eval-stockfish",
            }, separators=(",", ":")) + "\n")
            written += 1

    print(f"wrote {written} Stockfish-eval labels to {args.output}; skipped {skipped}")
    return 0 if written > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
