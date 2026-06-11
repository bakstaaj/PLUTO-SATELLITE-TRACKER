#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ -f "$ROOT_DIR/.pluto.env" ]]; then
  # shellcheck disable=SC1091
  source "$ROOT_DIR/.pluto.env"
fi

IMAGE="${PLUTO_CROSS_IMAGE:-pluto-adsb-tracker-cross:v0.39}"
HOST_ROOT="$(cygpath -w "$ROOT_DIR" | sed 's#\\#/#g')"
HOST_DOCKERFILE="$(cygpath -w "$ROOT_DIR/docker/Dockerfile.cross" | sed 's#\\#/#g')"

DOCKER="${DOCKER:-docker}"
if ! command -v "$DOCKER" >/dev/null 2>&1; then
  DOCKER_DESKTOP="/c/Program Files/Docker/Docker/resources/bin/docker.exe"
  if [[ -x "$DOCKER_DESKTOP" ]]; then
    DOCKER="$DOCKER_DESKTOP"
  fi
fi

NO_CACHE_ARGS=()
if [[ "${1:-}" == "--no-cache" ]]; then
  NO_CACHE_ARGS+=(--no-cache)
fi

echo "== Building Pluto v0.39 cross-compile image =="
echo "Image:      $IMAGE"
echo "Dockerfile: $HOST_DOCKERFILE"
echo

MSYS_NO_PATHCONV=1 "$DOCKER" build \
  "${NO_CACHE_ARGS[@]}" \
  -t "$IMAGE" \
  -f "$HOST_DOCKERFILE" \
  "$HOST_ROOT"

echo
echo "== Verifying cross compiler image =="
MSYS_NO_PATHCONV=1 "$DOCKER" run --rm "$IMAGE" bash -lc '
  set -e
  which arm-linux-gnueabihf-gcc
  arm-linux-gnueabihf-gcc --version | head -1
  test -d /opt/pluto/staging/usr/include
  echo "Pluto v0.39 sysroot present."
'

echo
echo "Docker cross-compile image is ready:"
echo "  $IMAGE"
