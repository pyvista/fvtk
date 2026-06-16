#!/usr/bin/env bash
#
# ci/build-wheels-linux.sh — build fvtk manylinux_2_17_x86_64 wheel(s) LOCALLY
# via a raw `docker run` inside quay.io/pypa/manylinux2014_x86_64.
#
# This is the NIX-FREE local mirror of .github/workflows/wheels-manylinux217.yml:
# the proven release recipe (glibc 2.17, devtoolset-10 GCC 10.2.1 + gold, el7 yum
# mesa, configure with ci/cmake/linux.cmake, cmake --build, generated build-tree
# `setup.py bdist_wheel`, then `auditwheel repair --plat manylinux_2_17_x86_64`).
#
# Unlike the local nix build (build-fvtk.sh), the container wheel is fully
# auditwheel-self-contained: no nix runtime libs, no LD_LIBRARY_PATH dance.
#
# Usage:
#   ci/build-wheels-linux.sh                 # default: cp313 only (fast)
#   ci/build-wheels-linux.sh 313             # explicit single version
#   ci/build-wheels-linux.sh 39 310 311 312 313   # full release matrix
#
# Env knobs:
#   IMAGE      manylinux image (default quay.io/pypa/manylinux2014_x86_64)
#   BUILD_JOBS parallel compile jobs (default: nproc)
#   CCACHE_DIR host ccache dir, mounted into the container so the python-
#              INDEPENDENT C++ kits are shared across the cp matrix AND across
#              re-runs (default: ~/.cache/fvtk-ccache-manylinux)
#   FVTK_LTO   1 (default) production LTO wheel; 0 for a fast iteration build
#   OUTDIR     host wheelhouse (default: <repo>/wheelhouse)
#
# All CPython legs run in ONE container invocation so they share the same ccache
# AND the same cmake build tree's compiled C++ objects across the loop (the
# second+ python only recompiles the python-version-dependent wrappers).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${IMAGE:-quay.io/pypa/manylinux2014_x86_64}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/fvtk-ccache-manylinux}"
OUTDIR="${OUTDIR:-$REPO/wheelhouse}"
FVTK_LTO="${FVTK_LTO:-1}"

PYVERS=("$@")
[ "${#PYVERS[@]}" -gt 0 ] || PYVERS=("313")

mkdir -p "$CCACHE_DIR" "$OUTDIR"

echo "=== fvtk manylinux2014 local docker build ==="
echo "  image     : $IMAGE"
echo "  pythons   : ${PYVERS[*]}"
echo "  jobs      : $BUILD_JOBS"
echo "  ccache    : $CCACHE_DIR (mounted -> /ccache)"
echo "  outdir    : $OUTDIR"
echo "  FVTK_LTO  : $FVTK_LTO"
echo

# The whole recipe runs as a single bash script inside the container. The repo is
# mounted read-only at /src and copied to a writable /work so the host tree stays
# pristine (the build tree, venvs and wheels are large and container-root-owned).
docker run --rm \
  -v "$REPO":/src:ro \
  -v "$CCACHE_DIR":/ccache \
  -v "$OUTDIR":/out \
  -e PYVERS="${PYVERS[*]}" \
  -e BUILD_JOBS="$BUILD_JOBS" \
  -e FVTK_LTO="$FVTK_LTO" \
  "$IMAGE" \
  bash -euxo pipefail -c '
    # ---- toolchain + el7 rendering deps (system mesa = GL backend) ----------
    # devtoolset-10 (GCC 10.2.1) + gold 2.35 + LTO plugin are already on PATH in
    # the manylinux2014 image. ccache from EPEL.
    yum install -y \
      mesa-libGL-devel mesa-libEGL-devel mesa-libOSMesa-devel \
      mesa-libGLU-devel libglvnd-devel \
      libX11-devel libXcursor-devel libXt-devel libXext-devel \
      ccache git
    gcc --version | head -1
    ld.gold --version | head -1
    ldd --version | head -1

    # ccache persisted on the host mount, shared across every cp leg + re-runs.
    export CCACHE_DIR=/ccache
    export CCACHE_MAXSIZE=30G
    ccache --zero-stats >/dev/null || true

    # Copy the read-only source to a writable work tree.
    cp -a /src /work
    cd /work
    git config --global --add safe.directory /work || true

    BUILD=/work/build
    FIRST_PY=""
    for V in $PYVERS; do
      PYBIN="/opt/python/cp${V}-cp${V}/bin"
      echo "===== cp${V} ====="
      "$PYBIN/python" -m pip install --upgrade pip
      "$PYBIN/python" -m pip install "cmake>=3.22,<4.2" ninja "setuptools<81" wheel auditwheel

      # CRITICAL: VTK derives the wrapper ABI suffix (EXT_SUFFIX/SOABI) from the
      # FIRST python3 on PATH, not from -DPython3_EXECUTABLE. Put THIS cp on PATH
      # so the wrappers are tagged for the right interpreter.
      export PATH="$PYBIN:$PATH"

      # Each python needs its OWN configured build tree (the wrappers + setup.py
      # are python-version-specific). But the ccache shares the python-independent
      # C++ kit objects across legs, so only the first leg pays the full C++ cost.
      rm -rf "$BUILD"
      FVTK_LTO="$FVTK_LTO" cmake -S /work -B "$BUILD" -G Ninja \
        -C ci/cmake/linux.cmake \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DPython3_EXECUTABLE="$PYBIN/python" \
        -DPython3_FIND_STRATEGY=LOCATION

      FVTK_LTO="$FVTK_LTO" cmake --build "$BUILD" --parallel "$BUILD_JOBS"
      ccache --show-stats | grep -Ei "hit|miss|cache size" || true

      # Wheel: prune dead UI subpackages, bdist_wheel from the build tree, repair.
      python /work/ci/prune_setup_py.py "$BUILD"
      ( cd "$BUILD" && python setup.py bdist_wheel )
      ls -la "$BUILD"/dist/*.whl
      auditwheel show "$BUILD"/dist/*.whl
      auditwheel repair --plat manylinux_2_17_x86_64 -w /out "$BUILD"/dist/*.whl
      if ! ls /out/*cp${V}*manylinux_2_17_x86_64*.whl >/dev/null 2>&1; then
        echo "ERROR: cp${V} repaired wheel is not tagged manylinux_2_17_x86_64" >&2
        exit 1
      fi
      [ -n "$FIRST_PY" ] || FIRST_PY="$V"
    done
    echo "=== ccache final ==="
    ccache --show-stats
    ls -la /out/*.whl
    # Make the host-owned outdir writable back (container runs as root).
    chmod -R a+rwX /out || true
  '

echo
echo "=== done. wheels in $OUTDIR: ==="
ls -la "$OUTDIR"/*.whl
