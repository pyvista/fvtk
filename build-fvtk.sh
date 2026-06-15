#!/usr/bin/env bash
#
# build-fvtk.sh — build the FULL 1:1 pyvista-parity fvtk wheel.
#
# Source: this VTK checkout (~/source/VTK, v9.6.2).
# Config: fvtk-config/{linux,_base,_modules}.cmake — the canonical CoDim parity
#         config: every module the stock pyvista `vtk` ships, INCLUDING the full
#         rendering stack (EGL + OSMesa + X/GLX + OpenGL2 + FreeType + Matplotlib
#         + Volume + Charts). NOT the core-only build-ultralight.sh.
#
# Two profiles:
#   FAST=1 (default) -> fvtk-config/fast.cmake : LTO off, ~25-40 min cold,
#                       minutes warm (ccache). For validation iteration.
#   FAST=0           -> fvtk-config/linux.cmake : LTO on, production wheel.
#
# Modeled on the proven build-ultralight.sh invocation (cmake -S/-B, --build,
# wheel venv) which builds cleanly on this box.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Rendering modules (RenderingOpenGL2 / EGL / OSMesa / X) are WANT under
# VTK_BUILD_ALL_MODULES, so if the OpenGL/EGL/OSMesa/X libraries aren't
# discoverable they SILENTLY drop and you get a core-only wheel with a clean
# configure. shell.nix provides libGL + mesa (EGL/OSMesa/GLX) + X11 on
# CMAKE_PREFIX_PATH. Re-exec inside it (once) so the rendering deps resolve.
# Set FVTK_IN_NIX_SHELL=1 to skip (already provisioned).
if [ "${FVTK_IN_NIX_SHELL:-0}" != "1" ]; then
  exec nix-shell "$REPO/shell.nix" --run "FVTK_IN_NIX_SHELL=1 PROFILE=${PROFILE:-minimal} FAST=${FAST:-1} USE_CCACHE=${USE_CCACHE:-1} FVTK_ICF=${FVTK_ICF:-1} FVTK_STRIP=${FVTK_STRIP:-0} FVTK_WRAP_OPTSIZE=${FVTK_WRAP_OPTSIZE:-1} FVTK_DISPATCH_MINIMAL=${FVTK_DISPATCH_MINIMAL:-1} FVTK_LTO=${FVTK_LTO:-1} FVTK_SEMINTERP=${FVTK_SEMINTERP:-1} FVTK_PGO=${FVTK_PGO:-} bash '${BASH_SOURCE[0]}'"
fi

BUILD="${BUILD:-$REPO/build-fvtk}"
# Absolutize a relative BUILD against the repo: the wheel step does `cd "$BUILD"`
# and then references "$BUILD/venv", so a relative BUILD would resolve twice.
[[ "$BUILD" = /* ]] || BUILD="$REPO/$BUILD"
CFG_DIR="$REPO/fvtk-config"
# PROFILE selects the module/build policy:
#   minimal (default) : deny-by-default, ONLY PyVista's measured closure
#                       (~74 modules, no .pyi, no LTO) — the lightest build.
#   fast              : CoDim BUILD_ALL_MODULES (whole VTK), LTO off.
#   linux             : CoDim BUILD_ALL_MODULES, LTO on (CoDim production wheel).
PROFILE="${PROFILE:-minimal}"
PROFILE_CFG="$CFG_DIR/${PROFILE}.cmake"
# Back-compat: FAST=0 selects the LTO production CoDim config.
[ "${PROFILE}" = "fast" ] && [ "${FAST:-1}" = "0" ] && PROFILE_CFG="$CFG_DIR/linux.cmake"
[ -f "$PROFILE_CFG" ] || { echo "no such profile config: $PROFILE_CFG" >&2; exit 1; }

# Toolchain: the nix gcc-wrapper the known-good ~/source/VTK/build used. The
# hardcoded store paths are this dev box's realizations; fall back to whatever
# nix-shell put on PATH (CI runners realize different store paths from the same
# shell.nix, so the pinned paths won't exist there).
CC_BIN="${CC:-/nix/store/myvv172x2am72534zgn9wx0qp5amq6a8-gcc-wrapper-14.3.0/bin/gcc}"
CXX_BIN="${CXX:-/nix/store/myvv172x2am72534zgn9wx0qp5amq6a8-gcc-wrapper-14.3.0/bin/g++}"
[ -x "$CC_BIN" ]  || CC_BIN="$(command -v gcc)"
[ -x "$CXX_BIN" ] || CXX_BIN="$(command -v g++)"

# Python 3.13 backing ~/.uvenv313 (the pyvista parity reference interpreter).
# Derive the prefix from python3.13 on PATH when the pinned store path is absent
# (CI). $PY313 is used below as bin/include/lib prefix, so resolve it from the
# interpreter's own prefix (python3.13 -> <prefix>/bin/python3.13).
PY313=/nix/store/cdaifv92znxy5ai4sawricjl0p5b9sgf-python3-3.13.11
if [ ! -x "$PY313/bin/python3.13" ]; then
  _py="$(command -v python3.13 || true)"
  [ -n "$_py" ] || { echo "no python3.13 on PATH (shell.nix should provide it)" >&2; exit 1; }
  PY313="$("$_py" -c 'import sys;print(sys.base_prefix)')"
fi

# CRITICAL: shell.nix puts python3.11 on PATH as `python3`. VTK's Python wrapper
# build derives the module ABI suffix (EXT_SUFFIX/SOABI) from the FIRST `python3`
# on PATH, not from -DPython3_EXECUTABLE — so without this shim the wrappers are
# named cpython-311 and a cp313 wheel ships unimportable modules. Force `python3`
# / `python` to resolve to the 3.13 interpreter for the whole build.
PYSHIM="$REPO/.pyshim313"
mkdir -p "$PYSHIM"
ln -sf "$PY313/bin/python3.13" "$PYSHIM/python3"
ln -sf "$PY313/bin/python3.13" "$PYSHIM/python"
export PATH="$PYSHIM:$PATH"

# Pin nix cmake 4.1.2 first on PATH so nested try_compile sub-builds don't grab a
# pip cmake 4.2.x (which regresses with "CMAKE_CXX_COMPILER not set").
CMAKE_BIN="${CMAKE_BIN:-/nix/store/g35r73rnpfxq5alf7jlyibl4j5lf222y-cmake-4.1.2/bin/cmake}"
[ -x "$CMAKE_BIN" ] || CMAKE_BIN="$(command -v cmake)"
export PATH="$(dirname "$CMAKE_BIN"):$PATH"

CMATH="${CMATH_LIBRARY:-$("$CC_BIN" -print-file-name=libm.so)}"

# ccache: cold full builds are large; warm rebuilds after a config tweak are
# minutes. Fetch from nix on demand. USE_CCACHE=0 to disable.
CCACHE_ARGS=()
if [ "${USE_CCACHE:-1}" = "1" ]; then
  CCACHE_BIN="${CCACHE_BIN:-$(command -v ccache || true)}"
  if [ -z "$CCACHE_BIN" ]; then
    for _p in $(nix build nixpkgs#ccache --no-link --print-out-paths 2>/dev/null); do
      [ -x "$_p/bin/ccache" ] && CCACHE_BIN="$_p/bin/ccache" && break
    done
  fi
  if [ -x "$CCACHE_BIN" ]; then
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/fvtk-ccache}"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-30G}"
    mkdir -p "$CCACHE_DIR"
    CCACHE_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER="$CCACHE_BIN" -DCMAKE_CXX_COMPILER_LAUNCHER="$CCACHE_BIN")
    echo "ccache: $CCACHE_BIN ($CCACHE_DIR)"
  fi
fi

export VTK_DISABLE_PYTHON_PROPERTIES=1

echo "=== configure ($([ "${FAST:-1}" = 0 ] && echo PRODUCTION+LTO || echo FAST/no-LTO)) using $PROFILE_CFG ==="
"$CMAKE_BIN" -S "$REPO" -B "$BUILD" -G Ninja \
  -C "$PROFILE_CFG" \
  "${CCACHE_ARGS[@]}" \
  -DCMAKE_C_COMPILER="$CC_BIN" \
  -DCMAKE_CXX_COMPILER="$CXX_BIN" \
  -DCMath_LIBRARY="$CMATH" \
  -DCMath_HAVE_LIBM_POW=TRUE \
  -DPython3_EXECUTABLE="$PY313/bin/python3.13" \
  -DPython3_INCLUDE_DIR="$PY313/include/python3.13" \
  -DPython3_LIBRARY="$PY313/lib/libpython3.13.so" \
  ${EXTRA_CMAKE_ARGS:-}

echo "=== build (${BUILD_JOBS:-8} jobs) ==="
"$CMAKE_BIN" --build "$BUILD" --parallel "${BUILD_JOBS:-8}"

# Wheel via a setuptools-capable venv (matches build-ultralight.sh).
WHEEL_VENV="$BUILD/venv"
if [ ! -x "$WHEEL_VENV/bin/python3" ]; then
  "$PY313/bin/python3.13" -m venv "$WHEEL_VENV"
  "$WHEEL_VENV/bin/python3" -m ensurepip --upgrade
fi
"$WHEEL_VENV/bin/python3" -m pip install --upgrade pip setuptools wheel >/dev/null

find "$BUILD" -path '*/fvtk/*.pyi' -delete 2>/dev/null || true

# Strip symbol tables from every shared object before bundling. A Release build
# still emits the full .symtab (local + non-dynamic symbols) into each .so —
# auditwheel/manylinux normally strips this, but we don't run auditwheel, so the
# unstripped wheel carries ~30-40% dead weight. We use --strip-all: on a SHARED
# object it removes .symtab + debug but KEEPS .dynsym (the dynamic symbol table
# needed to load/link the module), so it's runtime-safe and matches what stock
# vtk/manylinux wheels ship. (--strip-unneeded is too timid for a .so — it leaves
# .symtab almost intact: libvtkCommon 162MB unchanged vs 97MB under --strip-all.)
# Strip both the python wrapper .so and the libvtk*.so kits.
if [ "${FVTK_STRIP:-0}" = "1" ]; then
  STRIP_BIN="${STRIP_BIN:-$("$CC_BIN" -print-prog-name=strip 2>/dev/null)}"
  [ -x "$STRIP_BIN" ] || STRIP_BIN="$(command -v strip || true)"
  if [ -n "$STRIP_BIN" ] && [ -x "$STRIP_BIN" ]; then
    _n=0
    while IFS= read -r _so; do
      "$STRIP_BIN" --strip-all "$_so" 2>/dev/null && _n=$((_n+1)) || true
    done < <(find "$BUILD" \( -name '*.so' -o -name '*.so.*' \) 2>/dev/null | grep -E '/fvtk/[^/]*\.so(\.[0-9.]+)?$')
    echo "stripped $_n shared objects ($STRIP_BIN --strip-all)"
  else
    echo "WARN: no strip binary found; wheel ships unstripped" >&2
  fi
fi
# Drop the GTK/Qt/Tk/wx UI-interactor helper packages — pyvista never imports
# them (its Qt path is the separate pyvistaqt package). They're pure-Python so
# they cost nothing to build, but they're junk for a pyvista wheel. The
# generated setup.py hardcodes them in packages=[...]; remove both the dirs and
# the setup.py references so bdist_wheel doesn't fail on the missing dirs.
for sub in gtk qt test tk wx; do
  rm -rf "$BUILD/fvtk/$sub" "$BUILD"/build/lib.*/fvtk/$sub 2>/dev/null || true
done
if [ -f "$BUILD/setup.py" ]; then
  sed -i -E "/'fvtk\.(gtk|qt|tk|wx|test)',?/d" "$BUILD/setup.py"
fi

echo "=== wheel ==="
( cd "$BUILD" && "$WHEEL_VENV/bin/python3" setup.py bdist_wheel )

# Re-strip inside the finished wheel. The pre-bdist strip above only sees the
# cmake-staged Python wrappers under "$BUILD/fvtk"; the KIT libraries
# (libvtkCommon.so, libvtkFilters.so, ...) are copied into the wheel package by
# bdist_wheel AFTER that strip, so some ship unstripped — notably libvtkCommon.so,
# which carried a ~26 MB .symtab (~1.7 MiB off the compressed wheel). Unpack,
# strip every .so, and repack; `wheel pack` regenerates RECORD so the wheel stays
# install-valid.
if [ "${FVTK_STRIP:-0}" = "1" ] && [ -n "${STRIP_BIN:-}" ] && [ -x "${STRIP_BIN:-/nonexistent}" ]; then
  _whl="$(ls "$BUILD"/dist/*.whl)"
  _unp="$BUILD/_whlrestrip"; rm -rf "$_unp"; mkdir -p "$_unp"
  "$WHEEL_VENV/bin/python3" -m wheel unpack "$_whl" -d "$_unp"
  _n=0
  while IFS= read -r _so; do
    "$STRIP_BIN" --strip-all "$_so" 2>/dev/null && _n=$((_n+1)) || true
  done < <(find "$_unp" \( -name '*.so' -o -name '*.so.*' \))
  _pkgdir="$(find "$_unp" -mindepth 1 -maxdepth 1 -type d | head -1)"
  rm -f "$_whl"
  "$WHEEL_VENV/bin/python3" -m wheel pack "$_pkgdir" -d "$BUILD/dist"
  rm -rf "$_unp"
  echo "re-stripped $_n shared objects inside the wheel and repacked"
fi
ls -la "$BUILD"/dist/*.whl
