#!/usr/bin/env python3
"""Extract high-depth FENs from the Lichess evaluation JSONL dump."""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import chess


def normalize_fen(raw: str) -> str | None:
    fields = raw.split()
    if len(fields) == 4:
        fields.extend(["0", "1"])
    if len(fields) != 6:
        return None
    try:
        board = chess.Board(" ".join(fields))
    except ValueError:
        return None
    if board.is_game_over(claim_draw=True):
        return None
    return board.fen()


def deepest_eval(record: dict) -> dict | None:
    evals = record.get("evals")
    if not isinstance(evals, list) or not evals:
        return None
    best = None
    for item in evals:
        if not isinstance(item, dict):
            continue
        depth = item.get("depth")
        if not isinstance(depth, int):
            continue
        if best is None or depth > best.get("depth", -1):
            best = item
    return best


def first_pv(eval_record: dict) -> dict:
    pvs = eval_record.get("pvs")
    if isinstance(pvs, list) and pvs and isinstance(pvs[0], dict):
        return pvs[0]
    return {}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--meta-output", type=Path, default=None)
    parser.add_argument("--count", type=int, default=1_000_000)
    parser.add_argument("--min-depth", type=int, default=35)
    parser.add_argument("--unique", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--progress", type=int, default=100_000)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.meta_output:
        args.meta_output.parent.mkdir(parents=True, exist_ok=True)

    seen: set[str] = set()
    scanned = 0
    accepted = 0
    skipped_depth = 0
    skipped_bad = 0
    started = time.time()

    with args.output.open("w", encoding="utf-8") as fen_handle:
        meta_handle = args.meta_output.open("w", encoding="utf-8") if args.meta_output else None
        try:
            for raw in sys.stdin:
                scanned += 1
                try:
                    record = json.loads(raw)
                except json.JSONDecodeError:
                    skipped_bad += 1
                    continue

                eval_record = deepest_eval(record)
                if eval_record is None:
                    skipped_bad += 1
                    continue
                depth = int(eval_record["depth"])
                if depth < args.min_depth:
                    skipped_depth += 1
                    continue

                fen = normalize_fen(str(record.get("fen", "")))
                if fen is None:
                    skipped_bad += 1
                    continue
                if args.unique and fen in seen:
                    continue
                seen.add(fen)

                fen_handle.write(fen + "\n")
                if meta_handle:
                    pv = first_pv(eval_record)
                    meta_handle.write(json.dumps({
                        "fen": fen,
                        "depth": depth,
                        "knodes": eval_record.get("knodes"),
                        "line": pv.get("line", ""),
                        "cp": pv.get("cp"),
                        "mate": pv.get("mate"),
                    }, separators=(",", ":")) + "\n")

                accepted += 1
                if accepted % 1000 == 0:
                    fen_handle.flush()
                    if meta_handle:
                        meta_handle.flush()
                if args.progress > 0 and accepted % args.progress == 0:
                    elapsed = max(time.time() - started, 1e-6)
                    print(
                        f"accepted {accepted}/{args.count}; scanned {scanned}; "
                        f"depth_skip {skipped_depth}; bad_skip {skipped_bad}; "
                        f"{accepted / elapsed:.1f}/s",
                        file=sys.stderr,
                        flush=True,
                    )
                if accepted >= args.count:
                    break
        finally:
            if meta_handle:
                meta_handle.close()

    print(
        f"wrote {accepted} FENs to {args.output}; scanned {scanned}; "
        f"min_depth {args.min_depth}; skipped_depth {skipped_depth}; skipped_bad {skipped_bad}",
        file=sys.stderr,
    )
    return 0 if accepted >= args.count else 1


if __name__ == "__main__":
    raise SystemExit(main())
