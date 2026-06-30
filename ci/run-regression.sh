#!/usr/bin/env bash
#
# Regression gate: pure-Python behavior tests that exercise cvista's C++/wrapper
# fixes (e.g. ghost-map reentrancy, GIL release). Unlike bitexact/renderexact
# these do not compare against stock VTK; they assert cvista's own behavior on the
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

# Install the freshly built wheel FIRST with --no-index: WHEELDIR holds a
# pre-release (.devN) wheel, and a bare `cvista` requirement lets pip prefer a
# published PyPI release over it — silently testing the released version instead
# of this build. --no-index forces the local wheel (pip still tag-matches it);
# the second install resolves cvista's deps from PyPI without pulling the published
# release (cvista is already satisfied). The tests import `cvista` directly (no shim).
"$BASE_PY" -m venv /tmp/cvista-reg
/tmp/cvista-reg/bin/pip -q install --upgrade pip "numpy==2.4.6" pytest
/tmp/cvista-reg/bin/pip -q install --no-index --no-deps --find-links "$WHEELDIR" cvista
/tmp/cvista-reg/bin/pip -q install --find-links "$WHEELDIR" cvista

cd "$SRC/tests/regression"
/tmp/cvista-reg/bin/python -m pytest -v --tb=short -p no:cacheprovider
