#!/usr/bin/env python3
"""Download the strongest official Stockfish binary for this machine.

By default this selects the newest official development pre-release from
official-stockfish/Stockfish because that is usually stronger than the latest
stable release. Use --channel stable when reproducibility matters more.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path
from typing import Any


API_RELEASES = "https://api.github.com/repos/official-stockfish/Stockfish/releases"
TOOL_DIR = Path(__file__).resolve().parent
DEFAULT_BIN_DIR = TOOL_DIR / "bin"


def fetch_json(url: str) -> Any:
    req = urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "rinnegan-nnue-tools",
        },
    )
    with urllib.request.urlopen(req, timeout=30) as response:
        return json.loads(response.read().decode("utf-8"))


def cpu_flags() -> set[str]:
    flags: set[str] = set()
    system = platform.system()
    if system == "Linux":
        try:
            text = Path("/proc/cpuinfo").read_text(encoding="utf-8", errors="ignore")
        except OSError:
            return flags
        for line in text.splitlines():
            if line.lower().startswith(("flags", "features")):
                _, _, value = line.partition(":")
                flags.update(value.lower().split())
                break
    elif system == "Darwin":
        for key in ("machdep.cpu.features", "machdep.cpu.leaf7_features"):
            try:
                out = subprocess.check_output(
                    ["sysctl", "-n", key],
                    text=True,
                    stderr=subprocess.DEVNULL,
                ).strip()
            except (OSError, subprocess.CalledProcessError):
                continue
            flags.update(out.lower().split())
    return flags


def keyword_score(name: str, flags: set[str]) -> int:
    lower = name.lower()
    score = 0
    if "vnni512" in lower and ("avx512vnni" in flags or "avx512_vnni" in flags):
        score += 100
    if "avx512" in lower and any(flag.startswith("avx512") for flag in flags):
        score += 90
    if "bmi2" in lower and "bmi2" in flags:
        score += 80
    if "avx2" in lower and "avx2" in flags:
        score += 70
    if "sse41-popcnt" in lower and ("sse4.1" in flags or "sse4_1" in flags):
        score += 50
    if "dotprod" in lower:
        score += 40
    if "neon" in lower:
        score += 30
    if "x86-64" in lower or "x86_64" in lower:
        score += 10
    return score


def asset_score(asset_name: str, flags: set[str]) -> int:
    lower = asset_name.lower()
    if "source code" in lower or not lower.endswith((".tar", ".tar.gz", ".zip")):
        return -1

    system = platform.system()
    machine = platform.machine().lower()

    if system == "Darwin":
        if "macos" not in lower:
            return -1
        if machine in {"arm64", "aarch64"}:
            if not any(token in lower for token in ("m1", "apple-silicon", "arm")):
                return -1
            return 1000 + keyword_score(lower, flags)
        if "x86-64" not in lower and "x86_64" not in lower:
            return -1
        return 900 + keyword_score(lower, flags)

    if system == "Linux":
        if not any(token in lower for token in ("linux", "ubuntu")):
            return -1
        if machine in {"x86_64", "amd64"}:
            if "x86-64" not in lower and "x86_64" not in lower:
                return -1
            return 900 + keyword_score(lower, flags)
        if machine in {"aarch64", "arm64"}:
            if not any(token in lower for token in ("armv8", "aarch64", "arm64")):
                return -1
            return 800 + keyword_score(lower, flags)
        return -1

    if system == "Windows":
        if "windows" not in lower:
            return -1
        if machine in {"amd64", "x86_64"} and ("x86-64" in lower or "x86_64" in lower):
            return 900 + keyword_score(lower, flags)
        return -1

    return -1


def select_release(releases: list[dict[str, Any]], channel: str) -> dict[str, Any]:
    if channel == "dev":
        for release in releases:
            if release.get("prerelease") and release.get("assets"):
                return release
        raise RuntimeError("No official Stockfish development pre-release with assets was found")

    if channel == "stable":
        for release in releases:
            if not release.get("prerelease") and not release.get("draft") and release.get("assets"):
                return release
        raise RuntimeError("No official Stockfish stable release with assets was found")

    for release in releases:
        if release.get("assets"):
            return release
    raise RuntimeError("No official Stockfish release with assets was found")


def select_asset(release: dict[str, Any]) -> dict[str, Any]:
    flags = cpu_flags()
    scored: list[tuple[int, dict[str, Any]]] = []
    for asset in release.get("assets", []):
        score = asset_score(asset["name"], flags)
        if score >= 0:
            scored.append((score, asset))
    if not scored:
        names = ", ".join(asset["name"] for asset in release.get("assets", []))
        raise RuntimeError(f"No matching Stockfish binary asset for this platform. Assets: {names}")
    scored.sort(key=lambda item: item[0], reverse=True)
    return scored[0][1]


def download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    last_pct = [-1.0]

    def progress(blocks: int, block_size: int, total: int) -> None:
        if total <= 0:
            return
        done = min(blocks * block_size, total)
        pct = done * 100 / total
        if pct < 100.0 and pct - last_pct[0] < 5.0:
            return
        last_pct[0] = pct
        sys.stderr.write(f"\rDownloading Stockfish: {pct:5.1f}%")
        sys.stderr.flush()

    urllib.request.urlretrieve(url, dest, reporthook=progress)
    sys.stderr.write("\n")


def extract_archive(archive: Path, dest: Path) -> list[Path]:
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True, exist_ok=True)
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as zf:
            zf.extractall(dest)
    else:
        with tarfile.open(archive) as tf:
            tf.extractall(dest)
    return [path for path in dest.rglob("*") if path.is_file()]


def looks_like_engine(path: Path) -> bool:
    name = path.name.lower()
    if not name.startswith("stockfish"):
        return False
    if any(token in name for token in (".nnue", ".txt", ".md", ".json", ".sha")):
        return False
    if path.suffix.lower() in {".txt", ".md", ".nnue"}:
        return False
    return True


def install_engine(files: list[Path], bin_dir: Path) -> Path:
    candidates = [path for path in files if looks_like_engine(path)]
    if not candidates:
        raise RuntimeError("Archive did not contain a Stockfish executable")
    candidates.sort(key=lambda path: (len(path.name), str(path)))
    source = candidates[0]

    bin_dir.mkdir(parents=True, exist_ok=True)
    target = bin_dir / ("stockfish.exe" if platform.system() == "Windows" else "stockfish")
    if target.exists() or target.is_symlink():
        target.unlink()
    shutil.copy2(source, target)
    target.chmod(target.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return target


def smoke_uci(engine: Path) -> list[str]:
    proc = subprocess.run(
        [str(engine)],
        input="uci\nquit\n",
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=10,
        check=False,
    )
    lines = proc.stdout.splitlines()
    if not any(line.strip() == "uciok" for line in lines):
        raise RuntimeError("Downloaded Stockfish did not complete UCI initialization")
    return lines[:12]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--channel", choices=("dev", "stable", "latest"), default="dev")
    parser.add_argument("--bin-dir", type=Path, default=DEFAULT_BIN_DIR)
    parser.add_argument("--force", action="store_true", help="download again even if bin/stockfish exists")
    args = parser.parse_args()

    target = args.bin_dir / ("stockfish.exe" if platform.system() == "Windows" else "stockfish")
    if target.exists() and not args.force:
        print(target)
        return 0

    releases = fetch_json(API_RELEASES)
    release = select_release(releases, args.channel)
    asset = select_asset(release)

    with tempfile.TemporaryDirectory(prefix="rinnegan-stockfish-") as tmp:
        tmp_dir = Path(tmp)
        archive = tmp_dir / asset["name"]
        download(asset["browser_download_url"], archive)
        files = extract_archive(archive, tmp_dir / "extract")
        engine = install_engine(files, args.bin_dir)

    uci_lines = smoke_uci(engine)
    meta = {
        "release": release.get("name"),
        "tag": release.get("tag_name"),
        "channel": args.channel,
        "asset": asset["name"],
        "url": asset["browser_download_url"],
        "engine": str(engine),
        "uci_head": uci_lines,
    }
    (args.bin_dir / "stockfish.meta.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    print(engine)
    print(f"Installed {release.get('name')} from {asset['name']}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
