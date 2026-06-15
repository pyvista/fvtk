#!/usr/bin/env bash
# 3-phase PGO release build of fvtk: instrument -> train -> rebuild with profile.
#
# Self-contained for CI/release (no machine-specific paths). Phases:
#   1. instrument:  FVTK_PGO=gen (LTO/ICF off, unstripped) -> instrumented wheel
#   2. train:       run tools/pgo-train.py (contour-weighted + general hot paths)
#                   against the instrumented wheel inside the shell.nix GL env, so
#                   real branch/call frequencies land in .gcda next to the objects
#   3. rebuild:     FVTK_PGO=use + LTO + ICF + strip -> final profile-guided wheel
#
# gen and use MUST share $BUILD (the .gcda are keyed to object paths). We drop
# CMakeCache.txt between phases so the use-configure starts from clean base flags
# instead of accumulating gen's -fprofile-generate; the .gcda survive the wipe.
#
# Env: REPO (default cwd), BUILD, PYVISTA_DIR (training source; shallow-cloned if
# absent), JOBS (default nproc).
set -euo pipefail
REPO="${REPO:-$PWD}"
BUILD="${BUILD:-$REPO/build-fvtk-pgo}"
PYVISTA_DIR="${PYVISTA_DIR:-$REPO/_pyvista_train}"
JOBS="${JOBS:-$(nproc)}"
TRAIN_VENV="${TRAIN_VENV:-/tmp/fvtk-pgo-train-venv}"

echo "::group::PGO phase 1/3 — instrument build"
FVTK_PGO=gen FVTK_LTO=0 FVTK_ICF=0 FVTK_STRIP=0 BUILD="$BUILD" BUILD_JOBS="$JOBS" "$REPO/build-fvtk.sh"
WHEEL_GEN="$(ls "$BUILD"/dist/*.whl)"
echo "instrumented wheel: $WHEEL_GEN"
echo "::endgroup::"

[ -d "$PYVISTA_DIR" ] || git clone --depth 1 https://github.com/pyvista/pyvista "$PYVISTA_DIR"
find "$BUILD" -name '*.gcda' -delete 2>/dev/null || true

echo "::group::PGO phase 2/3 — train"
# Run training inside shell.nix so mesa/EGL/libstdc++ resolve for offscreen render.
WHEEL_GEN="$WHEEL_GEN" PYVISTA_DIR="$PYVISTA_DIR" TRAIN_VENV="$TRAIN_VENV" REPO="$REPO" \
# Training is GL-free (tools/pgo-train.py renders nothing), so we only need the
# loader path for the wheel + numpy to IMPORT (libstdc++ from gcc, libz etc. from
# the nix buildInputs) — no EGL/OSMesa setup, which is what broke headless CI.
nix-shell "$REPO/shell.nix" --run '
  set -e
  for p in $buildInputs; do LD_LIBRARY_PATH="$p/lib:${LD_LIBRARY_PATH:-}"; done
  LD_LIBRARY_PATH="$(dirname "$(g++ -print-file-name=libstdc++.so.6)"):$LD_LIBRARY_PATH"
  export LD_LIBRARY_PATH
  export PYVISTA_OFF_SCREEN=true
  python3.13 -m venv "$TRAIN_VENV"
  "$TRAIN_VENV/bin/python" -m pip -q install --upgrade pip numpy
  "$TRAIN_VENV/bin/python" -m pip -q install "$PYVISTA_DIR"
  # pyvista pulls stock `vtk` as a dep; remove it so the shim cannot silently
  # train against stock VTK — any un-redirected vtkmodules import must fail loud.
  "$TRAIN_VENV/bin/python" -m pip -q uninstall -y vtk 2>/dev/null || true
  "$TRAIN_VENV/bin/python" -m pip -q install "$WHEEL_GEN"
  # redirect vtkmodules.* -> fvtk.* so pyvista drives the instrumented wheel
  SP="$("$TRAIN_VENV/bin/python" -c "import site;print(site.getsitepackages()[0])")"
  cp "$REPO/tools/fvtk_shim.py" "$SP/_fvtk_shim.py"
  echo "import _fvtk_shim" > "$SP/_fvtk_shim.pth"
  "$TRAIN_VENV/bin/python" "$REPO/tools/pgo-train.py"
'
echo "::endgroup::"

N=$(find "$BUILD" -name '*.gcda' | wc -l)
echo "profile data: $N .gcda files"
[ "$N" -ge 100 ] || { echo "ERROR: profile too sparse ($N) — training did not exercise the build"; exit 1; }

echo "::group::PGO phase 3/3 — rebuild with profile (LTO+ICF+strip)"
rm -f "$BUILD/CMakeCache.txt"
FVTK_PGO=use FVTK_LTO=1 FVTK_ICF=1 FVTK_STRIP=1 BUILD="$BUILD" BUILD_JOBS="$JOBS" "$REPO/build-fvtk.sh"
echo "::endgroup::"

echo "PGO release wheel: $(ls "$BUILD"/dist/*.whl)"
