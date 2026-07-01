#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

docker_path() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -w "$1" | sed 's#\\#/#g'
  else
    printf '%s\n' "$1"
  fi
}

if [[ -f "$ROOT_DIR/.pluto.env" ]]; then
  # shellcheck disable=SC1091
  source "$ROOT_DIR/.pluto.env"
fi

IMAGE="${PLUTO_CROSS_IMAGE:-pluto-adsb-tracker-cross:v0.39}"
HOST_ROOT="$(docker_path "$ROOT_DIR")"

DOCKER="${DOCKER:-docker}"
if ! command -v "$DOCKER" >/dev/null 2>&1; then
  DOCKER_DESKTOP="/c/Program Files/Docker/Docker/resources/bin/docker.exe"
  if [[ -x "$DOCKER_DESKTOP" ]]; then
    DOCKER="$DOCKER_DESKTOP"
  fi
fi

REBUILD_IMAGE=0
CLEAN_BUILD=0

usage() {
  cat <<USAGE
Usage: $0 [options]

Options:
  --rebuild-image   Rebuild the Docker toolchain/sysroot image first.
  --clean           Run make clean before compiling.
  --help            Show this help.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rebuild-image)
      REBUILD_IMAGE=1
      ;;
    --clean)
      CLEAN_BUILD=1
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      usage
      exit 1
      ;;
  esac
  shift
done

cd "$ROOT_DIR"

if [[ "$REBUILD_IMAGE" -eq 1 ]]; then
  "$ROOT_DIR/tools/build_cross_image.sh"
elif ! "$DOCKER" image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "Docker image $IMAGE does not exist. Building it now."
  "$ROOT_DIR/tools/build_cross_image.sh"
else
  echo "Using existing Docker cross-compile image:"
  echo "  $IMAGE"
fi

echo
echo "== Cross-compiling Pluto Satellite Tracker =="

CLEAN_COMMAND=":"
if [[ "$CLEAN_BUILD" -eq 1 ]]; then
  CLEAN_COMMAND="make clean || true"
fi

MSYS_NO_PATHCONV=1 "$DOCKER" run --rm \
  -v "${HOST_ROOT}:/work" \
  -w /work \
  "$IMAGE" \
  bash -lc "
    set -euo pipefail

    export TOOLCHAIN_DIR=/opt/toolchains/gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabihf
    export PATH=\"\$TOOLCHAIN_DIR/bin:\$PATH\"
    export CC=arm-linux-gnueabihf-gcc
    export PLUTO_SYSROOT=/opt/pluto/staging

    echo '== Compiler =='
    arm-linux-gnueabihf-gcc --version | head -1
    echo

    $CLEAN_COMMAND

    make \
      CC=arm-linux-gnueabihf-gcc \
      CFLAGS=\"--sysroot=/opt/pluto/staging -I/opt/pluto/staging/usr/include -O2 -g -Wall -Wextra -std=c99\" \
      LDFLAGS=\"--sysroot=/opt/pluto/staging -L/opt/pluto/staging/usr/lib -pthread\"

    mkdir -p dist
    cp pluto_sat_tracker dist/pluto_sat_tracker
    cp pluto_fm_receiver dist/pluto_fm_receiver
    cp pluto_digital_decoder dist/pluto_digital_decoder
    cp pluto_test_gen dist/pluto_test_gen
    arm-linux-gnueabihf-strip dist/pluto_sat_tracker || true
    arm-linux-gnueabihf-strip dist/pluto_fm_receiver || true
    arm-linux-gnueabihf-strip dist/pluto_digital_decoder || true
    arm-linux-gnueabihf-strip dist/pluto_test_gen || true

    echo
    echo '== Built binaries =='
    file dist/pluto_sat_tracker
    file dist/pluto_fm_receiver
    file dist/pluto_digital_decoder
    file dist/pluto_test_gen
    ls -lh dist/pluto_sat_tracker dist/pluto_fm_receiver dist/pluto_digital_decoder dist/pluto_test_gen
  "

echo
echo "Build complete:"
echo "  $ROOT_DIR/dist/pluto_sat_tracker"
echo "  $ROOT_DIR/dist/pluto_digital_decoder"
