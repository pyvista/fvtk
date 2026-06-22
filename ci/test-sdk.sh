#!/usr/bin/env bash
#
# Smoke-test the built fvtk-sdk wheel: install it and confirm a downstream
# scikit-build-core project resolves VTK via `find_package(VTK CONFIG)` through
# the `cmake.prefix` entry point. Exercises the wheel_sdks test suite copied into
# the build tree by vtkWheelPreparation. Runs on Linux, macOS, and Windows.
#
#   Usage: ci/test-sdk.sh   (expects ci/build-sdk.sh to have produced ./sdk-dist)
set -euxo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${SRC}/build-sdk}"
OUT="${OUT:-${SRC}/sdk-dist}"

case "$(uname -s)" in
  Linux*)               FVTK_OS=linux;   PYBIN="${PYBIN:-/opt/python/cp312-cp312/bin}" ;;
  Darwin*)              FVTK_OS=macos;   PYBIN="${PYBIN:-$(dirname "$(command -v python3)")}" ;;
  MINGW*|MSYS*|CYGWIN*) FVTK_OS=windows; PYBIN="${PYBIN:-$(dirname "$(command -v python)")}" ;;
  *) echo "test-sdk: unsupported OS $(uname -s)" >&2; exit 1 ;;
esac

# pip / pytest are native programs; on Windows they need drive-letter paths, not
# the MSYS `/c/...` form. cygpath -m emits the form they accept; no-op elsewhere.
cmpath() {
  if [ "$FVTK_OS" = windows ]; then cygpath -m "$1"; else printf '%s' "$1"; fi
}

export PATH="$PYBIN:$PATH"

# pip via the module (Windows pip.exe is in Scripts/, not next to python.exe).
"$PYBIN/python" -m pip install -U pip pytest virtualenv

# package-version parity check (importlib.metadata vs __version__)
"$PYBIN/python" -m pip install --no-index --find-links "$(cmpath "$OUT")" fvtk-sdk
"$PYBIN/python" -m pytest "$(cmpath "$BUILD_DIR/wheel_sdks/tests/test_package.py")" -v

# find_package(VTK) integration: the test builds a downstream project that
# depends on fvtk-sdk; run it from the wheel dir so its `--find-links .` resolves
# the freshly built fvtk-sdk wheel.
cd "$OUT"
"$PYBIN/python" -m pytest "$(cmpath "$BUILD_DIR/wheel_sdks/tests/test_find_package.py")" -v

# out-of-tree module build + Python-wrap: the test builds a downstream project
# that find_package(VTK COMPONENTS ... WrappingPythonCore) + vtk_module_scan +
# vtk_module_build + vtk_module_wrap_python against the same fvtk-sdk wheel, then
# asserts the wrapped extension was produced. Proves the SDK supports compiling
# AND wrapping an out-of-tree module, per the README's downstream-SDK intent.
# Same wheel dir so its `--find-links .` resolves the freshly built fvtk-sdk wheel.
"$PYBIN/python" -m pytest "$(cmpath "$BUILD_DIR/wheel_sdks/tests/test_wrap_module.py")" -v
