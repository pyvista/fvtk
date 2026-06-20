#!/usr/bin/env bash
#
# Regression gate: pure-Python behavior tests that exercise fvtk's C++/wrapper
# fixes (e.g. ghost-map reentrancy, GIL release). Unlike bitexact/renderexact
# these do not compare against stock VTK; they assert fvtk's own behavior on the
# built wheel. Runs on any host with a tag-compatible python — the wheel is a
# self-contained manylinux wheel, so no container is needed.
#
# Some tests render (the GIL probes), so the host needs a GL backend. As in
# run-renderexact.sh we force Mesa software EGL (llvmpipe / surfaceless), the only
# EGL device on ubuntu-latest; the workflow installs the runtime libs
# (libegl1 libgl1-mesa-dri libglx-mesa0 libgles2 libosmesa6 libxcb1).
#
#   Usage: ci/run-regression.sh <wheel-dir> [base-python]   (base-python default python3)
set -euxo pipefail

WHEELDIR="${1:?usage: ci/run-regression.sh <wheel-dir> [base-python]}"
BASE_PY="${2:-python3}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Force Mesa software EGL so the factory vtkRenderWindow gets a context with no
# display or GPU (matches run-renderexact.sh).
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export EGL_PLATFORM=surfaceless
export VTK_DEFAULT_OPENGL_WINDOW=vtkEGLRenderWindow
export VTK_EGL_DEVICE_INDEX="${VTK_EGL_DEVICE_INDEX:-0}"

# fvtk wheel in its own venv. --find-links points pip at the local dir to
# resolve `fvtk` (tag-compatible pick) while PyPI stays available for fvtk's
# declared deps — so NO --no-index. The tests import `fvtk` directly (no shim).
"$BASE_PY" -m venv /tmp/fvtk-reg
/tmp/fvtk-reg/bin/pip -q install --upgrade pip "numpy==2.4.6" pytest
/tmp/fvtk-reg/bin/pip -q install --find-links "$WHEELDIR" fvtk

cd "$SRC/tests/regression"
/tmp/fvtk-reg/bin/python -m pytest -v --tb=short -p no:cacheprovider
