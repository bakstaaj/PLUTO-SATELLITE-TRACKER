#!/usr/bin/env bash
# Diagnose Pluto dual RX capability v2.
#
# No SSH required. Uses host-side libiio against PLUTO_IIO_URI or ip:$PLUTO_IP.
#
# Output:
#   /c/Users/jim/Downloads/pluto_dual_rx_capability_report_v2.txt
#
# Run:
#   ./tools/diagnose_pluto_dual_rx_capability_v2.sh .

set +e

ROOT="${1:-.}"

if [[ -f "$ROOT/.pluto.env" ]]; then
  # shellcheck disable=SC1090
  source "$ROOT/.pluto.env"
fi

PLUTO_IP="${PLUTO_IP:-192.168.2.1}"
PLUTO_IIO_URI="${PLUTO_IIO_URI:-ip:${PLUTO_IP}}"

OUT_DIR="/c/Users/jim/Downloads"
if [[ ! -d "$OUT_DIR" ]]; then
  OUT_DIR="."
fi

REPORT="$OUT_DIR/pluto_dual_rx_capability_report_v2.txt"
TMP_DIR="${TMPDIR:-/tmp}"

capture_one() {
  local label="$1"
  shift
  local outfile="$TMP_DIR/pluto_dual_rx_${label}.raw"
  local errfile="$TMP_DIR/pluto_dual_rx_${label}.err"

  rm -f "$outfile" "$errfile"

  echo "===== Capture ${label} ====="
  echo "CMD: iio_readdev -u ${PLUTO_IIO_URI} -b 4096 -s 4096 cf-ad9361-lpc $*"

  if ! command -v iio_readdev >/dev/null 2>&1; then
    echo "STATUS: missing_iio_readdev"
    echo "BYTES: 0"
    echo
    return
  fi

  if command -v timeout >/dev/null 2>&1; then
    timeout 10 iio_readdev -u "$PLUTO_IIO_URI" -b 4096 -s 4096 cf-ad9361-lpc "$@" >"$outfile" 2>"$errfile"
    local status=$?
  else
    iio_readdev -u "$PLUTO_IIO_URI" -b 4096 -s 4096 cf-ad9361-lpc "$@" >"$outfile" 2>"$errfile"
    local status=$?
  fi

  local bytes=0
  if [[ -f "$outfile" ]]; then
    bytes="$(wc -c < "$outfile" 2>/dev/null)"
  fi

  echo "STATUS: $status"
  echo "BYTES: ${bytes:-0}"

  if [[ -s "$errfile" ]]; then
    echo "STDERR:"
    sed -n '1,120p' "$errfile"
  fi

  rm -f "$outfile" "$errfile"
  echo
}

{
  echo "Pluto dual RX capability report v2"
  echo "Generated UTC: $(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || date)"
  echo "Repo root: $(cd "$ROOT" 2>/dev/null && pwd || echo "$ROOT")"
  echo "PLUTO_IP: $PLUTO_IP"
  echo "PLUTO_IIO_URI: $PLUTO_IIO_URI"
  echo

  echo "===== Host tool availability ====="
  for tool in iio_info iio_readdev timeout wc sed; do
    if command -v "$tool" >/dev/null 2>&1; then
      echo "TOOL ${tool}: present ($(command -v "$tool"))"
    else
      echo "TOOL ${tool}: missing"
    fi
  done
  echo

  echo "===== iio_info context ====="
  if command -v iio_info >/dev/null 2>&1; then
    iio_info -u "$PLUTO_IIO_URI" 2>&1 | sed -n '1,320p'
    echo "IIO_INFO_STATUS: ${PIPESTATUS[0]}"
  else
    echo "iio_info missing"
    echo "IIO_INFO_STATUS: missing"
  fi
  echo

  echo "===== RX channel hints ====="
  if command -v iio_info >/dev/null 2>&1; then
    iio_info -u "$PLUTO_IIO_URI" 2>&1 | sed -n '/cf-ad9361-lpc/,+90p' | sed -n '/voltage[01]/p;/index/p;/format/p;/enabled/p'
  fi
  echo

  capture_one rx0 voltage0
  capture_one rx1 voltage1
  capture_one rx0_rx1 voltage0 voltage1

  echo "===== Interpretation note ====="
  echo "PASS on RX0, RX1, and RX0+RX1 means raw dual-RX capture is available."
  echo "The backend can then be patched for rx_channel=0|1|best, where best evaluates RX0/RX1 signal quality and selects the better one while using one shared Doppler-tracked LO."
} > "$REPORT"

python - "$REPORT" <<'PY'
import re
import sys
from pathlib import Path

report = Path(sys.argv[1])
text = report.read_text(encoding="utf-8", errors="replace")
lower = text.lower()

def capture_bytes(label):
    m = re.search(rf"===== Capture {re.escape(label)} =====.*?BYTES:\s*(\d+)", text, re.S)
    return int(m.group(1)) if m else 0

checks = [
    ("iio_info present", "tool iio_info: present" in lower),
    ("iio_readdev present", "tool iio_readdev: present" in lower),
    ("cf-ad9361-lpc present", "cf-ad9361-lpc" in lower),
    ("voltage0 mentioned", re.search(r"voltage0", text, re.I) is not None),
    ("voltage1 mentioned", re.search(r"voltage1", text, re.I) is not None),
    ("RX0 capture bytes", capture_bytes("rx0") > 0),
    ("RX1 capture bytes", capture_bytes("rx1") > 0),
    ("RX0+RX1 capture bytes", capture_bytes("rx0_rx1") > 0),
]

print("===== Host PASS/FAIL summary =====")
failed = False
for name, ok in checks:
    detail = ""
    if name == "RX0 capture bytes":
        detail = f" ({capture_bytes('rx0')} bytes)"
    elif name == "RX1 capture bytes":
        detail = f" ({capture_bytes('rx1')} bytes)"
    elif name == "RX0+RX1 capture bytes":
        detail = f" ({capture_bytes('rx0_rx1')} bytes)"
    print(("PASS: " if ok else "FAIL: ") + name + detail)
    failed = failed or not ok

print(f"\nReport written: {report}")
sys.exit(1 if failed else 0)
PY
