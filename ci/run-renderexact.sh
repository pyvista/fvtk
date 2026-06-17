#!/usr/bin/env bash
#
# Pixel-exact RENDER gate: assert the built fvtk wheel renders BYTE-FOR-BYTE
# identical framebuffers to stock VTK 9.6.2 through the SAME software GL driver.
#
# Sister to ci/run-bitexact.sh, but for the rendering pipeline (tests/renderexact)
# instead of the numeric filters (tests/bitexact). Both backends render the same
# vtkmodules-only scenes offscreen via vtkEGLRenderWindow and we diff the RGBA +
# Z buffers pixel-for-pixel. CRITICAL CONTROL: both sides must hit the *same* GL
# driver, so we force Mesa software (llvmpipe / surfaceless EGL) on the host and
# the harness asserts GL_RENDERER/GL_VERSION matched before trusting any diff.
#
# Designed for ubuntu-latest CI (no NVIDIA): the only EGL device is Mesa software
# at index 0, so VTK_EGL_DEVICE_INDEX=0 + EGL_PLATFORM=surfaceless is correct.
# (The local NVIDIA box needs index 1 to skip its GPU; that override is host-only.)
#
#   Usage: ci/run-renderexact.sh <wheel-dir> [base-python]   (base-python default python3)
set -euxo pipefail

WHEELDIR="${1:?usage: ci/run-renderexact.sh <wheel-dir> [base-python]}"
BASE_PY="${2:-python3}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

OUT="${RENDEREXACT_OUTDIR:-/tmp/rx-out}"
rm -rf "$OUT"
mkdir -p "$OUT/stock" "$OUT/fvtk"

# Force Mesa software EGL so BOTH backends render against an identical driver.
# On ubuntu-latest llvmpipe is the only (and therefore device-0) EGL device.
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export EGL_PLATFORM=surfaceless
export VTK_DEFAULT_OPENGL_WINDOW=vtkEGLRenderWindow
export VTK_EGL_DEVICE_INDEX="${VTK_EGL_DEVICE_INDEX:-0}"

# stock VTK 9.6.2
"$BASE_PY" -m venv /tmp/rx-stock
/tmp/rx-stock/bin/pip -q install --upgrade pip
/tmp/rx-stock/bin/pip -q install "numpy==2.4.6" "vtk==9.6.2"

# fvtk wheel + vtkmodules->fvtk redirect shim
"$BASE_PY" -m venv /tmp/rx-fvtk
/tmp/rx-fvtk/bin/pip -q install --upgrade pip "numpy==2.4.6"
/tmp/rx-fvtk/bin/pip -q install "$WHEELDIR"/*.whl
SP=$(/tmp/rx-fvtk/bin/python -c 'import sysconfig;print(sysconfig.get_paths()["purelib"])')
cp "$SRC/tools/fvtk_shim.py" "$SP/_fvtk_shim.py"
echo "import _fvtk_shim" > "$SP/_fvtk_shim.pth"

cd "$SRC/tests/renderexact"

# Render every scene under each backend (the vtkmodules import resolves per-venv).
/tmp/rx-stock/bin/python run_render.py "$OUT/stock"
/tmp/rx-fvtk/bin/python  run_render.py "$OUT/fvtk"

# Pixel-exact diff + GL-driver-match gate (exits non-zero on any diff / mismatch).
# Use the stock venv's python — compare_render.py needs numpy.
/tmp/rx-stock/bin/python compare_render.py "$OUT/stock" "$OUT/fvtk"
