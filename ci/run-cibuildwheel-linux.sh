#!/usr/bin/env bash
#
# ci/run-cibuildwheel-linux.sh — drive cibuildwheel locally via docker for cvista.
#
# Mirrors what the pypa/cibuildwheel GHA action does on CI, but on THIS box:
# cibuildwheel orchestrates the manylinux_2_28 container itself (AlmaLinux 8 mesa
# via CIBW_BEFORE_ALL, cmake build via the cvista_backend, auditwheel repair to
# manylinux_2_28, smoke test under xvfb).
#
# Usage:
#   ci/run-cibuildwheel-linux.sh                       # default: cp312+ abi3 matrix
#   ci/run-cibuildwheel-linux.sh cp312-*               # just the abi3 leg
#
# Python 3.11 has been dropped: the backend builds ONE cp312-abi3 wheel; cp312+
# all dedup to it (cibuildwheel's abi3 dedup reuses it for cp313/cp314). The
# default selector is the cp312..cp314 matrix, which yields a SINGLE abi3 wheel.
# Set CVISTA_ABI3=0 to force a legacy static per-version wheel (escape hatch).
#
# Requires a cibuildwheel on PATH (or in a venv): pip install cibuildwheel.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTDIR="${OUTDIR:-$REPO/wheelhouse-cibw}"
SELECTOR="${1:-cp312-* cp313-* cp314-*}"

mkdir -p "$OUTDIR"

CIBW="${CIBW:-cibuildwheel}"
command -v "$CIBW" >/dev/null 2>&1 || CIBW="/tmp/cibw-venv/bin/cibuildwheel"

echo "=== cibuildwheel local (linux) ==="
echo "  selector : $SELECTOR"
echo "  outdir   : $OUTDIR"
echo "  ccache   : in-container /ccache (shared across the cp matrix within the run)"

# --only takes a single identifier; --build/CIBW_BUILD takes a glob. Use the env
# var so a multi-selector ("cp39-* cp313-*") works too.
CIBW_BUILD="$SELECTOR" \
  "$CIBW" --platform linux --output-dir "$OUTDIR" "$REPO"

echo "=== wheels ==="
ls -la "$OUTDIR"/*.whl
