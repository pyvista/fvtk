#!/usr/bin/env bash
#
# PyVista parity gate: run PyVista's OWN full test suite against the built fvtk
# wheel, with `vtkmodules.*` redirected to `fvtk.*`. This proves the contract
# downstream actually depends on — not just our internal bit-exact / pixel-exact
# gates, but PyVista's thousands of behavioral + plotting assertions driving the
# fvtk graphics + data stack.
#
# Sister to ci/run-bitexact.sh / ci/run-renderexact.sh, but instead of our own
# vtkmodules-only scenes it installs PyVista (pinned to tests/pyvista/PYVISTA_REF)
# and runs its suite. ONE venv: stock `vtk` is NEVER installed, so any import
# that slips past the fvtk shim fails loud (ModuleNotFoundError) rather than
# silently testing stock VTK — never a false green.
#
# Heavy by design (full suite, software GL). NOT on the PR fast path — see the
# `pyvista` job in .github/workflows/ci.yml (merge queue / dispatch / labeled PR).
#
#   Usage: ci/run-pyvista.sh <wheel-dir> [base-python]   (base-python default python3)
set -euxo pipefail

WHEELDIR="${1:?usage: ci/run-pyvista.sh <wheel-dir> [base-python]}"
BASE_PY="${2:-python3}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HERE="$SRC/tests/pyvista"

REF="$(tr -d '[:space:]' < "$HERE/PYVISTA_REF")"
[ -n "$REF" ] || { echo "ERROR: tests/pyvista/PYVISTA_REF is empty"; exit 1; }

OUT="${PYVISTA_OUTDIR:-/tmp/pv-out}"
rm -rf "$OUT"; mkdir -p "$OUT"

# Headless software GL via Xvfb — the SAME path PyVista's own CI uses to render
# (and to generate its committed image_cache): an Xvfb virtual display + Mesa
# llvmpipe over GLX. We deliberately do NOT force surfaceless EGL here (unlike
# run-renderexact.sh, which drives raw vtkmodules): PyVista warns + segfault-
# guards when no DISPLAY is present, runs filterwarnings=error, and its image
# cache is GLX/llvmpipe — so matching that environment is what keeps the suite
# green. We wrap each pytest invocation in xvfb-run below; here we only pin
# software rendering and keep every Plotter offscreen.
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export PYVISTA_OFF_SCREEN=true
# Provides a real DISPLAY (silences _warn_xserver) + a fresh server per call.
XVFB=(xvfb-run -a -s "-screen 0 1280x1024x24")

# --- fetch PyVista at the pinned SHA (shallow, single commit) ----------------
# Pinning (not floating main) keeps the gate deterministic; .github/workflows/
# pyvista-bump.yml advances the pin through a self-validating PR. The committed
# image_cache that PyVista's plotting tests diff against rides along with the
# checkout.
PVDIR="${PYVISTA_SRC:-/tmp/pyvista-src}"
if [ ! -e "$PVDIR/pyproject.toml" ]; then
    rm -rf "$PVDIR"; mkdir -p "$PVDIR"
    git -C "$PVDIR" init -q
    git -C "$PVDIR" remote add origin https://github.com/pyvista/pyvista
    git -C "$PVDIR" fetch -q --depth 1 origin "$REF"
    git -C "$PVDIR" checkout -q FETCH_HEAD
fi
echo ">>> PyVista @ $(git -C "$PVDIR" rev-parse --short HEAD) (pinned $REF)"

# --- venv: PyVista (no deps) + test group + fvtk wheel + redirect shim -------
"$BASE_PY" -m venv /tmp/pv
/tmp/pv/bin/pip -q install --upgrade pip
# Non-editable: the clone dir is named `pyvista`, which an editable install
# mistakes for a namespace package. --no-deps so PyVista does NOT drag stock
# `vtk` into the venv (the shim must be the ONLY vtkmodules provider).
/tmp/pv/bin/pip -q install --no-deps "$PVDIR"
# PyVista's `test` dependency-group (pytest, pytest-xdist, pytest-pyvista, ...).
# pytest-timeout is NOT in that group, so add it explicitly for --timeout.
/tmp/pv/bin/pip -q install --group "$PVDIR/pyproject.toml:test" pytest-timeout
# Built fvtk wheel. --find-links resolves `fvtk` from the local dir while PyPI
# stays available for fvtk's own deps (matplotlib/numpy/...) — so NO --no-index.
/tmp/pv/bin/pip -q install --find-links "$WHEELDIR" fvtk
# vtkmodules.* -> fvtk.* redirect, active at interpreter startup via sitecustomize
# (.pth). Same shim the bit-exact / PGO harnesses use.
SP=$(/tmp/pv/bin/python -c 'import sysconfig;print(sysconfig.get_paths()["purelib"])')
cp "$SRC/tools/fvtk_shim.py" "$SP/_fvtk_shim.py"
echo "import _fvtk_shim" > "$SP/_fvtk_shim.pth"
# Put the venv's bin on PATH: we invoke python by absolute path (no `activate`),
# so without this the CLI tests that shell out to the bare `pyvista`/`pytest`
# console-scripts get FileNotFoundError.
export PATH="/tmp/pv/bin:$PATH"

# --- identity: prove PyVista is driving fvtk through the shim ----------------
/tmp/pv/bin/python - <<'PY'
import vtkmodules
assert "fvtk" in (vtkmodules.__file__ or ""), \
    f"vtkmodules NOT redirected to fvtk (got {vtkmodules.__file__!r}) -- shim failed"
import fvtk, pyvista
from vtkmodules.vtkCommonCore import vtkVersion
print("vtkmodules ->", vtkmodules.__file__)
print("fvtk       ->", fvtk.__file__)
print("VTK version->", vtkVersion.GetVTKVersion())
print("pyvista    ->", pyvista.__version__, pyvista.__file__)
PY

# --- deselect list -----------------------------------------------------------
# Static triage entries from tests/pyvista/deselect.txt (one nodeid per line).
DESELECT=()
while IFS= read -r raw || [ -n "$raw" ]; do
    line="${raw%%#*}"                          # strip comment
    line="${line#"${line%%[![:space:]]*}"}"    # ltrim
    line="${line%"${line##*[![:space:]]}"}"    # rtrim
    [ -n "$line" ] && DESELECT+=(--deselect "$line")
done < "$HERE/deselect.txt"

# snake_case opt-out: if this fvtk wheel was built with VTK_DISABLE_PYTHON_
# PROPERTIES, VTK emits no `obj.snake_case` descriptors and PyVista's snake_case
# tests fail BY DESIGN, not by regression. Probe at runtime and deselect only
# then, so the gate still exercises them against a wheel built WITH properties.
SNAKE_DISABLED="$(/tmp/pv/bin/python -c 'import vtkmodules.vtkCommonCore as m; print("0" if hasattr(m.vtkObject(), "global_warning_display") else "1")')"
if [ "$SNAKE_DISABLED" = "1" ]; then
    echo ">>> snake_case API disabled in this wheel: deselecting PyVista snake_case tests"
    DESELECT+=(
        --deselect "tests/test_attributes.py::test_vtk_snake_case_api_is_disabled"
        --deselect "tests/test_attributes.py::test_dir_snake_case_visible_when_allowed"
        --deselect "tests/core/test_utilities.py::test_vtk_snake_case"
        --deselect "tests/core/test_utilities.py::test_is_vtk_attribute"
        # Same root cause: with no snake_case property descriptors, PyVista cannot
        # tell which CamelCase names are VTK-inherited, so its dir() filtering
        # (hide-inherited / opt-in-show) can't engage. Fails by design, not by
        # regression — bare nodeids deselect every parametrization.
        --deselect "tests/test_attributes.py::test_dir_hides_vtk_inherited_attributes"
        --deselect "tests/test_attributes.py::test_dir_show_vtk_api_opt_in"
    )
else
    echo ">>> snake_case API present in this wheel: running PyVista snake_case tests"
fi

cd "$PVDIR"
# Two SEPARATE pytest invocations matching PyVista's tox layout (test-core,
# test-plotting). One xdist run over the whole tree cross-pollutes core fixtures
# into plotting workers and triggers spurious check_gc teardown errors.
#   -n auto                 : full xdist parallelism within each env
#   -m "not needs_download" : skip network-flaky tests (array element, not a
#                             string — a quoted string splits into broken tokens)
#   --timeout=120           : hang guard per test
SHARED=(-n auto --tb=short --no-header -ra --color=no -m "not needs_download" --timeout=120)

set +e
echo "=== core tests ==="
"${XVFB[@]}" /tmp/pv/bin/python -m pytest "${SHARED[@]}" "${DESELECT[@]+"${DESELECT[@]}"}" \
    --ignore=tests/plotting \
    --ignore=tests/typing \
    --test_downloads \
    --junitxml="$OUT/junit-core.xml" \
    tests/ 2>&1 | tee "$OUT/pytest-core.log"
STATUS_CORE=${PIPESTATUS[0]}

echo "=== plotting tests ==="
"${XVFB[@]}" /tmp/pv/bin/python -m pytest "${SHARED[@]}" "${DESELECT[@]+"${DESELECT[@]}"}" \
    --disallow_unused_cache \
    --junitxml="$OUT/junit-plotting.xml" \
    tests/plotting 2>&1 | tee "$OUT/pytest-plotting.log"
STATUS_PLOT=${PIPESTATUS[0]}
set -e

echo ">>> logs:  $OUT/pytest-{core,plotting}.log"
echo ">>> junit: $OUT/junit-{core,plotting}.xml"
exit $(( STATUS_CORE | STATUS_PLOT ))
