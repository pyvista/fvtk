#!/usr/bin/env bash
# CIBW_BEFORE_BUILD (per cp leg): provide the cmake/ninja the backend needs.
#
# pip's cmake wheel is >=3.22 and pip's ninja is >=1.11 (el7's ninja-build 1.10
# errors "multiple outputs aren't (yet?) supported by depslog" on VTK's wrapping
# edges). cmake pinned <4.2 (4.1.x ok; VTK 9.6.2 declares ...4.0 compat).
set -euxo pipefail
# setuptools_scm (the backend's tag-driven version source) runs `git` against the
# mounted source tree; in the manylinux container the checkout is owned by a
# different uid, so git refuses with "dubious ownership" unless the path is marked
# safe. Without this the version derivation silently falls back to dev0 instead of
# the release tag. (macOS/Windows build natively, same uid, so they don't need it.)
git config --global --add safe.directory '*' || true
python -m pip install --upgrade pip
python -m pip install "cmake>=3.22,<4.2" "ninja>=1.11" "setuptools<81" wheel
cmake --version | sed -n 1p   # sed reads whole stream; head closes early -> SIGPIPE under pipefail
ninja --version
ccache --zero-stats || true
