#!/usr/bin/env python3
"""Extract a policy NPZ into standalone NPY files for memory-mapped training."""

from __future__ import annotations

import argparse
import io
import json
import shutil
import zipfile
from pathlib import Path

import numpy as np


CORE_MEMBERS = [
    "features.npy",
    "group_offsets.npy",
    "target_indices.npy",
    "bucket_flags.npy",
]


def copy_member(archive: zipfile.ZipFile, name: str, output: Path) -> bool:
    if name not in archive.namelist():
        return False
    with archive.open(name) as source, (output / name).open("wb") as dest:
        shutil.copyfileobj(source, dest, length=16 * 1024 * 1024)
    return True


def read_meta(archive: zipfile.ZipFile, input_path: Path) -> dict:
    sidecar = input_path.with_suffix(input_path.suffix + ".meta.json")
    if sidecar.exists():
        return json.loads(sidecar.read_text(encoding="utf-8"))
    if "meta.npy" not in archive.namelist():
        return {}
    with archive.open("meta.npy") as source:
        raw = source.read()
    value = np.load(io.BytesIO(raw), allow_pickle=False)
    if isinstance(value, np.ndarray):
        value = value.item()
    try:
        return json.loads(str(value))
    except json.JSONDecodeError:
        return {"raw_meta": str(value)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--labels", action=argparse.BooleanOptionalAction, default=False,
                        help="Also extract labels.npy. The trainer does not need it.")
    parser.add_argument("--overwrite", action=argparse.BooleanOptionalAction, default=False)
    args = parser.parse_args()

    if args.output.exists() and any(args.output.iterdir()) and not args.overwrite:
        raise RuntimeError(f"output directory is not empty: {args.output}")

    args.output.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.input) as archive:
        members = CORE_MEMBERS + (["labels.npy"] if args.labels else [])
        copied = []
        for member in members:
            if copy_member(archive, member, args.output):
                copied.append(member)
        missing = [member for member in CORE_MEMBERS[:3] if member not in copied]
        if missing:
            raise RuntimeError(f"missing required dataset members: {', '.join(missing)}")
        meta = read_meta(archive, args.input)

    meta.setdefault("source_npz", str(args.input))
    meta.setdefault("materialized_format", "npy-dir")
    (args.output / "meta.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    print(f"materialized {', '.join(copied)} to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
