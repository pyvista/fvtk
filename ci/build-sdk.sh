#!/usr/bin/env bash
#
# Build the fvtk install tree and the matching `fvtk-sdk` wheel on Linux, macOS,
# or Windows.
#
# The SDK wheel ships the VTK C++ headers, CMake config, and wrap tools from the
# same source as the runtime `fvtk` wheel, wrapped as a scikit-build-core
# `cmake.prefix` package so a downstream `pip install fvtk-sdk` + `find_package(VTK)`
# just works. `CMake/vtkWheelPreparation.cmake` configures
# <build>/wheel_sdks/pyproject.toml at configure time, pointing VTK_INSTALL_DIR at
# CMAKE_INSTALL_PREFIX; this script supplies that prefix, populates it with
# `cmake --install`, then builds the wheel from <build>/wheel_sdks.
#
# Per-OS toolchain mirrors the runtime wheel build (ci.yml -> the matching
# [tool.cibuildwheel.<os>] section): the same CMake init-cache
# (ci/cmake/<os>.cmake) and the same macOS deployment target. The SDK ships the
# VTK libs UNVENDORED (no auditwheel/delocate/delvewheel repair) so downstream
# `find_package(VTK)` links them by their real SONAMEs, so the only post-build
# step is the Linux platform-tag relabel (PyPI rejects a raw `linux_x86_64`).
#
#   Linux   : runs inside quay.io/pypa/manylinux_2_28_x86_64
#   macOS   : arm64, MACOSX_DEPLOYMENT_TARGET=11.0
#   Windows : x64, MSVC on PATH (vcvars)
#
# The abi3 SDK is python-version-independent (wheel.py-api = "py3"), so one wheel
# per OS serves 3.12+.
#
#   Usage: ci/build-sdk.sh   (outputs the wheel to ./sdk-dist)
set -euxo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${SRC}/build-sdk}"
INSTALL_PREFIX="${INSTALL_PREFIX:-${SRC}/sdk-install}"
OUT="${OUT:-${SRC}/sdk-dist}"

# --- per-OS toolchain selection -------------------------------------------
# OS detection drives the CMake init-cache, the interpreter location, the macOS
# deployment target, and (Linux only) the manylinux platform-tag relabel.
case "$(uname -s)" in
  Linux*)
    FVTK_OS=linux
    INIT_CACHE="$SRC/ci/cmake/linux.cmake"
    PYBIN="${PYBIN:-/opt/python/cp312-cp312/bin}"
    ;;
  Darwin*)
    FVTK_OS=macos
    INIT_CACHE="$SRC/ci/cmake/macos.cmake"
    PYBIN="${PYBIN:-$(dirname "$(command -v python3)")}"
    export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
    ;;
  MINGW*|MSYS*|CYGWIN*)
    FVTK_OS=windows
    INIT_CACHE="$SRC/ci/cmake/windows.cmake"
    PYBIN="${PYBIN:-$(dirname "$(command -v python)")}"
    # vtkWrappingPythonCore is built Py_LIMITED_API and links the stable-ABI
    # python3.lib by bare name; FindPython only resolves the versioned
    # python3XX.lib (absolute path). Put the interpreter's libs/ dir on the MSVC
    # linker search path (LIB) so link.exe finds python3.lib (else LNK1104).
    export LIB="$(cygpath -w "$PYBIN/libs")${LIB:+;$LIB}"
    ;;
  *)
    echo "build-sdk: unsupported OS $(uname -s)" >&2
    exit 1
    ;;
esac

# CMake on Windows wants native (drive-letter) paths; cygpath -m emits the
# forward-slash `C:/...` form CMake and pip accept. No-op elsewhere.
cmpath() {
  if [ "$FVTK_OS" = windows ]; then cygpath -m "$1"; else printf '%s' "$1"; fi
}

# Invoke pip via the module, not a `$PYBIN/pip` shim: on Windows pip.exe lives in
# Scripts/ (not next to python.exe), so the shim path would not exist.
"$PYBIN/python" -m pip install -U pip cmake ninja "setuptools_scm>=8" wheel twine

# Put the pip cmake + ninja and the cpython on PATH so CMake's Ninja generator
# resolves CMAKE_MAKE_PROGRAM and the system compilers.
export PATH="$PYBIN:$PATH"

# Version suffix straight from the runtime wheel's backend so the SDK wheel
# version matches fvtk exactly (it applies the repo's setuptools_scm config:
# guess-next-dev, no-local-version, the 9.6.2 base and fallback). _version_suffix()
# prints a diagnostic to stdout, so capture only the returned value.
SUFFIX="$("$PYBIN/python" -c "
import sys, io, contextlib
sys.path.insert(0, r'$(cmpath "$SRC")/ci/cibw')
import fvtk_backend
with contextlib.redirect_stdout(io.StringIO()):
    s = fvtk_backend._version_suffix()
print(s)
")"

# LTO-off / -O2 fast config for the gate (the SDK content is headers + config +
# tools, not optimizer-sensitive); release uses the same script with FVTK_LTO
# unset for the shipped tools.
export FVTK_LTO="${FVTK_LTO:-0}"
export FVTK_GATE_O2="${FVTK_GATE_O2:-1}"

# ccache is present on the Linux (dnf) and macOS (brew) legs but not Windows;
# only wire the launcher when the binary actually exists, else CMake errors out
# trying to exec a missing `ccache`.
if command -v ccache >/dev/null 2>&1; then
  export CMAKE_C_COMPILER_LAUNCHER="${CMAKE_C_COMPILER_LAUNCHER:-ccache}"
  export CMAKE_CXX_COMPILER_LAUNCHER="${CMAKE_CXX_COMPILER_LAUNCHER:-ccache}"
fi

cmake -S "$(cmpath "$SRC")" -B "$(cmpath "$BUILD_DIR")" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$(cmpath "$(command -v ninja)")" \
    -C "$(cmpath "$INIT_CACHE")" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$(cmpath "$INSTALL_PREFIX")" \
    -DVTK_VERSION_SUFFIX="$SUFFIX"

JOBS="${FVTK_BUILD_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-4}")}"
cmake --build "$(cmpath "$BUILD_DIR")" --parallel "$JOBS"
cmake --install "$(cmpath "$BUILD_DIR")"

# Build the fvtk-sdk wheel from the build-tree scaffold (pyproject.toml was
# configured there by vtkWheelPreparation with the install prefix baked in).
rm -rf "$OUT"
"$PYBIN/python" -m pip wheel "$(cmpath "$BUILD_DIR/wheel_sdks")" --no-deps -w "$(cmpath "$OUT")"

# scikit-build-core stamps the wheel py3-none (wheel.py-api in
# CMake/wheel_sdks/pyproject.toml.in) plus the build platform. macOS
# (macosx_11_0_arm64) and Windows (win_amd64) tags are PyPI-valid as-is, but the
# Linux tag is a raw `linux_<arch>`, which PyPI rejects (400 "unsupported
# platform tag"). The Linux build runs inside the manylinux_2_28 image for its
# arch (x86_64 or aarch64), so relabel to manylinux_2_28_<arch>. We deliberately
# do NOT `auditwheel repair`: the SDK exposes the VTK shared/import libs
# unvendored so downstream `find_package(VTK)` links them by their real SONAMEs,
# and repair would rewrite those with hashed names and break that contract.
if [ "$FVTK_OS" = linux ]; then
  ARCH="$(uname -m)"  # x86_64 | aarch64
  WHEEL="$(ls "$OUT"/fvtk_sdk-*-linux_"$ARCH".whl)"
  "$PYBIN/python" -m wheel tags --platform-tag "manylinux_2_28_$ARCH" --remove "$WHEEL"
  if compgen -G "$OUT/*-linux_$ARCH.whl" >/dev/null; then
    echo "::error::fvtk-sdk wheel still has a raw linux_$ARCH platform tag; PyPI will 400 on upload"
    exit 1
  fi
fi

# twine's metadata/tag validation, same as the publish job will run.
( cd "$OUT" && "$PYBIN/python" -m twine check ./*.whl )

ls -lh "$OUT"
