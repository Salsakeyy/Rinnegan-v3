#!/usr/bin/env bash
# Build and package a CCRL-facing Rinnegan release bundle.

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
TOP="$(cd "$ROOT/.." && pwd)"

VERSION="v5.3"
ENGINE_NAME="Rinnegan-${VERSION}"
BUILD_DIR="$ROOT/build-ccrl"
RELEASE_DIR="$ROOT/release"
PLATFORM=""
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

usage() {
    cat <<EOF
Usage: tools/package-ccrl.sh [--platform NAME] [--jobs N] [--no-build]

Builds a Release binary with portable AVX2 flags on x86_64 and creates:
  release/${ENGINE_NAME}-<platform>.zip
EOF
}

DO_BUILD=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --platform) PLATFORM="$2"; shift 2 ;;
        --jobs|-j) JOBS="$2"; shift 2 ;;
        --no-build) DO_BUILD=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown arg: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$PLATFORM" ]]; then
    os="$(uname -s | tr '[:upper:]' '[:lower:]')"
    arch="$(uname -m)"
    case "$os" in
        darwin) os="macos" ;;
        mingw*|msys*|cygwin*) os="windows" ;;
        linux) os="linux" ;;
    esac
    case "$arch" in
        x86_64|amd64) arch="x86_64-avx2" ;;
        arm64|aarch64) arch="arm64" ;;
    esac
    PLATFORM="${os}-${arch}"
fi

if [[ "$DO_BUILD" -eq 1 ]]; then
    cmake -S "$ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DRINNEGAN_LTO=ON \
        -DRINNEGAN_NATIVE=OFF
    cmake --build "$BUILD_DIR" --target engine perft -j "$JOBS"
fi

ENGINE_SRC="$BUILD_DIR/engine"
EXE_SUFFIX=""
if [[ -x "$BUILD_DIR/engine.exe" ]]; then
    ENGINE_SRC="$BUILD_DIR/engine.exe"
    EXE_SUFFIX=".exe"
elif [[ ! -x "$ENGINE_SRC" ]]; then
    echo "ERROR: built engine not found in $BUILD_DIR" >&2
    exit 1
fi

PACKAGE_NAME="${ENGINE_NAME}-${PLATFORM}"
PACKAGE_DIR="$RELEASE_DIR/$PACKAGE_NAME"
rm -rf "$PACKAGE_DIR"
mkdir -p "$PACKAGE_DIR/source"

cp "$ENGINE_SRC" "$PACKAGE_DIR/${PACKAGE_NAME}${EXE_SUFFIX}"

cp "$ROOT/CMakeLists.txt" "$PACKAGE_DIR/source/"
if [[ -f "$ROOT/README.md" ]]; then
    cp "$ROOT/README.md" "$PACKAGE_DIR/source/"
else
    cp "$TOP/README.md" "$PACKAGE_DIR/source/"
fi
cp "$ROOT/CCRL_SUBMISSION.md" "$PACKAGE_DIR/source/"
cp -R "$ROOT/src" "$PACKAGE_DIR/source/src"
mkdir -p "$PACKAGE_DIR/source/tests" "$PACKAGE_DIR/source/tools"
cp "$ROOT/tests/perft_main.cpp" "$PACKAGE_DIR/source/tests/"
cp "$ROOT/tools/openbench-build.sh" "$PACKAGE_DIR/source/tools/"
cp "$ROOT/tools/package-ccrl.sh" "$PACKAGE_DIR/source/tools/"

cat > "$PACKAGE_DIR/README_CCRL.txt" <<EOF
Rinnegan v5.3
Author: Lorenzo Bodmer
Protocol: UCI

Disclosure:
  Rinnegan is fully written by Claude and Codex as a proof of concept.

Executable:
  ${PACKAGE_NAME}${EXE_SUFFIX}

Recommended CCRL options:
  Threads = 1 for 1CPU testing
  Hash = 256 or 512 MB, matching the tournament
  Ponder = off

No own opening book, no learning, no tablebase probing.

Verification:
  printf 'uci\nisready\nquit\n' | ./${PACKAGE_NAME}${EXE_SUFFIX}
  printf 'bench\nquit\n' | ./${PACKAGE_NAME}${EXE_SUFFIX}

Expected v5.3 bench:
  Nodes: 3136877

Source snapshot is included in ./source.
EOF

(
    cd "$RELEASE_DIR"
    rm -f "$PACKAGE_NAME.zip"
    zip -qr "$PACKAGE_NAME.zip" "$PACKAGE_NAME"
)

echo "$RELEASE_DIR/$PACKAGE_NAME.zip"
