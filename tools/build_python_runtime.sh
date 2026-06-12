#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ -f "$ROOT_DIR/.pluto.env" ]]; then
  # shellcheck disable=SC1091
  source "$ROOT_DIR/.pluto.env"
fi

IMAGE="${PLUTO_CROSS_IMAGE:-pluto-adsb-tracker-cross:v0.39}"
PYTHON_VERSION="${PLUTO_PYTHON_VERSION:-3.11.9}"
PYTHON_TARBALL="Python-${PYTHON_VERSION}.tgz"
PYTHON_URL="https://www.python.org/ftp/python/${PYTHON_VERSION}/${PYTHON_TARBALL}"

HOST_ROOT="$(cygpath -w "$ROOT_DIR" | sed 's#\\#/#g')"
DOCKER="${DOCKER:-docker}"
if ! command -v "$DOCKER" >/dev/null 2>&1; then
  DOCKER_DESKTOP="/c/Program Files/Docker/Docker/resources/bin/docker.exe"
  if [[ -x "$DOCKER_DESKTOP" ]]; then
    DOCKER="$DOCKER_DESKTOP"
  fi
fi

usage() {
  cat <<USAGE
Usage: $0 [options]

Build an ARM hard-float Python runtime for the Pluto SD card.

Options:
  --version VERSION   CPython version, default ${PYTHON_VERSION}.
  --help              Show this help.

Output:
  runtime/python-pluto-armhf.tar.gz
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version)
      PYTHON_VERSION="$2"
      PYTHON_TARBALL="Python-${PYTHON_VERSION}.tgz"
      PYTHON_URL="https://www.python.org/ftp/python/${PYTHON_VERSION}/${PYTHON_TARBALL}"
      shift 2
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
done

cd "$ROOT_DIR"
mkdir -p downloads runtime

if [[ ! -f "downloads/${PYTHON_TARBALL}" ]]; then
  echo "Downloading ${PYTHON_URL}"
  if command -v curl >/dev/null 2>&1; then
    curl -L --fail -o "downloads/${PYTHON_TARBALL}.tmp" "$PYTHON_URL"
  else
    wget -O "downloads/${PYTHON_TARBALL}.tmp" "$PYTHON_URL"
  fi
  mv "downloads/${PYTHON_TARBALL}.tmp" "downloads/${PYTHON_TARBALL}"
fi

MSYS_NO_PATHCONV=1 "$DOCKER" run --rm \
  -v "${HOST_ROOT}:/work" \
  -w /work \
  "$IMAGE" \
  bash -lc "
    set -euo pipefail
    BUILD_DIR=/tmp/python-runtime-build
    INSTALL_DIR=/tmp/python-runtime-install
    rm -rf \"\$BUILD_DIR\" \"\$INSTALL_DIR\" /work/runtime/python-pluto-armhf.tmp
    mkdir -p \"\$BUILD_DIR\"
    cd \"\$BUILD_DIR\"
    tar -xzf /work/downloads/${PYTHON_TARBALL}

    cp -a Python-${PYTHON_VERSION} Python-${PYTHON_VERSION}-host
    cd Python-${PYTHON_VERSION}-host
    env -u CC -u CXX -u AR -u RANLIB -u READELF \
      CC=gcc \
      ./configure --prefix=/tmp/python-build-host --without-ensurepip
    make -j\"\$(nproc)\" python
    if [ -f \"\$PWD/python.exe\" ] && [ -x \"\$PWD/python.exe\" ]; then
      HOST_PYTHON=\"\$PWD/python.exe\"
    elif [ -f \"\$PWD/python\" ] && [ -x \"\$PWD/python\" ]; then
      HOST_PYTHON=\"\$PWD/python\"
    else
      echo \"Unable to locate host build Python binary\" >&2
      exit 1
    fi

    cd ../Python-${PYTHON_VERSION}
    sed -i 's/^use_lfs=yes$/use_lfs=no/' configure
    cat > config.site <<'CONFIG_SITE'
ac_cv_file__dev_ptmx=yes
ac_cv_file__dev_ptc=no
ac_cv_func_memfd_create=no
CONFIG_SITE

    export CONFIG_SITE=\"\$PWD/config.site\"
    export CC=arm-linux-gnueabihf-gcc
    export AR=arm-linux-gnueabihf-ar
    export RANLIB=arm-linux-gnueabihf-ranlib
    export READELF=arm-linux-gnueabihf-readelf
    export CFLAGS=\"--sysroot=/opt/pluto/staging -I/opt/pluto/staging/usr/include -O2\"
    export CPPFLAGS=\"--sysroot=/opt/pluto/staging -I/opt/pluto/staging/usr/include\"
    export LDFLAGS=\"--sysroot=/opt/pluto/staging -L/opt/pluto/staging/usr/lib -Wl,-rpath,\\\$ORIGIN/../lib\"
    export ZLIB_CFLAGS=\"-I/opt/pluto/staging/usr/include\"
    export ZLIB_LIBS=\"-L/opt/pluto/staging/usr/lib -lz\"
    export PKG_CONFIG_SYSROOT_DIR=/opt/pluto/staging
    export PKG_CONFIG_LIBDIR=/opt/pluto/staging/usr/lib/pkgconfig:/opt/pluto/staging/usr/share/pkgconfig

    ./configure \
      --host=arm-linux-gnueabihf \
      --build=x86_64-pc-linux-gnu \
      --prefix=/python-runtime \
      --with-build-python=\"\$HOST_PYTHON\" \
      --without-ensurepip \
      --disable-ipv6 \
      --disable-test-modules

    make -j\"\$(nproc)\"
    make install DESTDIR=\"\$INSTALL_DIR\"

    cd \"\$INSTALL_DIR/python-runtime\"
    find . -name '__pycache__' -type d -prune -exec rm -rf '{}' +
    find . -name '*.pyc' -delete
    while IFS= read -r link_path; do
      link_target=\$(readlink \"\$link_path\")
      rm \"\$link_path\"
      cp -a \"\$(dirname \"\$link_path\")/\$link_target\" \"\$link_path\"
    done < <(find . -type l)
    rm -rf share

    cd lib
    cp -a /opt/pluto/staging/usr/lib/libz.so.1* . 2>/dev/null || true

    mkdir -p /work/runtime/python-pluto-armhf.tmp
    cp -a \"\$INSTALL_DIR/python-runtime\" /work/runtime/python-pluto-armhf.tmp/
  "

rm -rf runtime/python-pluto-armhf
mv runtime/python-pluto-armhf.tmp/python-runtime runtime/python-pluto-armhf
rm -rf runtime/python-pluto-armhf.tmp

tar -C runtime/python-pluto-armhf -czf runtime/python-pluto-armhf.tar.gz .

echo
echo "Built:"
ls -lh runtime/python-pluto-armhf.tar.gz
