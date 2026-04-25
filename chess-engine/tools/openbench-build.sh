#!/usr/bin/env bash
# Build a Rinnegan binary with LTO + (optionally) PGO for OpenBench / release.
#
# Usage:
#   tools/openbench-build.sh                           # LTO only, default name 'rinnegan'
#   tools/openbench-build.sh -o engine_v5              # LTO only, custom name
#   tools/openbench-build.sh --pgo                     # 2-pass PGO + LTO
#   tools/openbench-build.sh --pgo -o rinnegan-v5      # 2-pass PGO + LTO, custom name
#   tools/openbench-build.sh --no-lto                  # Plain build (debugging baseline)
#
# OpenBench typically calls this via `make openbench` or directly.
# Output binary lives in the repo root (alongside this directory's parent).

set -euo pipefail

cd "$(dirname "$0")/.."
REPO_ROOT="$(pwd)"

OUTPUT_NAME="rinnegan"
ENABLE_PGO=0
ENABLE_LTO=1
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -o|--output)   OUTPUT_NAME="$2"; shift 2 ;;
        --pgo)         ENABLE_PGO=1; shift ;;
        --no-lto)      ENABLE_LTO=0; shift ;;
        -j)            JOBS="$2"; shift 2 ;;
        -h|--help)     sed -n '1,15p' "$0"; exit 0 ;;
        *)             echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

LTO_FLAG="-DRINNEGAN_LTO=$([[ $ENABLE_LTO -eq 1 ]] && echo ON || echo OFF)"

# Detect compiler family for PGO orchestration (Clang vs GCC).
CXX="${CXX:-c++}"
COMPILER_ID=""
if "$CXX" --version 2>&1 | grep -qiE 'clang|apple llvm'; then
    COMPILER_ID="clang"
elif "$CXX" --version 2>&1 | grep -qi 'gcc'; then
    COMPILER_ID="gcc"
else
    COMPILER_ID="unknown"
fi

configure_with() {
    local pgo_mode="$1"
    local build_dir="$2"
    rm -rf "$build_dir"
    cmake -B "$build_dir" -S "$REPO_ROOT" \
          -DCMAKE_BUILD_TYPE=Release \
          "$LTO_FLAG" \
          -DRINNEGAN_PGO="$pgo_mode" \
          >/dev/null
}

build_target() {
    local build_dir="$1"
    cmake --build "$build_dir" -j "$JOBS" --target engine
}

build_with() {
    configure_with "$1" "$2"
    build_target "$2"
}

run_bench_workload() {
    local engine="$1"
    printf 'bench\nquit\n' | "$engine" >/dev/null
}

run_pgo_workload() {
    local engine="$1"
    # pgo-train is wider than bench and capped by nodes per FEN to keep
    # instrumented builds predictable.
    printf 'pgo-train\nquit\n' | "$engine" >/dev/null
}

if [[ $ENABLE_PGO -eq 0 ]]; then
    echo "[openbench-build] LTO=$ENABLE_LTO PGO=off"
    build_with off build
    cp "build/engine" "$OUTPUT_NAME"
else
    if [[ "$COMPILER_ID" == "unknown" ]]; then
        echo "Cannot detect compiler family; PGO requires clang or gcc." >&2
        exit 3
    fi

    echo "[openbench-build] LTO=$ENABLE_LTO PGO=2-pass compiler=$COMPILER_ID"

    # Pass 1: instrumented build.
    build_with generate build-pgo
    PROFILE_DIR="build-pgo/pgo"
    mkdir -p "$PROFILE_DIR"

    # On Clang, instrumented binaries write LLVM_PROFILE_FILE; point it at the dir.
    if [[ "$COMPILER_ID" == "clang" ]]; then
        export LLVM_PROFILE_FILE="$REPO_ROOT/$PROFILE_DIR/raw-%p.profraw"
    fi

    echo "[openbench-build] running pgo-train to generate profile data..."
    run_pgo_workload "build-pgo/engine"

    # Clang: merge .profraw -> .profdata.
    if [[ "$COMPILER_ID" == "clang" ]]; then
        if command -v llvm-profdata >/dev/null 2>&1; then
            PROFDATA="llvm-profdata"
        elif command -v xcrun >/dev/null 2>&1; then
            PROFDATA="xcrun llvm-profdata"
        else
            echo "llvm-profdata not found; install it (xcode CLT, or llvm package)." >&2
            exit 4
        fi
        echo "[openbench-build] merging profile data..."
        $PROFDATA merge -output="$PROFILE_DIR/merged.profdata" "$PROFILE_DIR"/raw-*.profraw
    fi

    # Pass 2: configure, place profile data where cmake's -fprofile-use expects it, then build.
    configure_with use build-pgo-final
    if [[ "$COMPILER_ID" == "clang" ]]; then
        mkdir -p "build-pgo-final/pgo"
        cp "$PROFILE_DIR/merged.profdata" "build-pgo-final/pgo/merged.profdata"
    else
        # GCC: copy the .gcda files into the new build dir so -fprofile-use can find them.
        mkdir -p "build-pgo-final/pgo"
        cp -r "$PROFILE_DIR"/. "build-pgo-final/pgo/"
    fi
    build_target build-pgo-final
    cp "build-pgo-final/engine" "$OUTPUT_NAME"
fi

echo "[openbench-build] done -> $OUTPUT_NAME"
printf 'bench\nquit\n' | "./$OUTPUT_NAME" | tail -1
