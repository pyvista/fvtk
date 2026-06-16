#!/usr/bin/env bash
# CIBW_BEFORE_BUILD (per cp leg): provide the cmake/ninja the backend needs.
#
# pip's cmake wheel is >=3.22 and pip's ninja is >=1.11 (el7's ninja-build 1.10
# errors "multiple outputs aren't (yet?) supported by depslog" on VTK's wrapping
# edges). cmake pinned <4.2 (4.1.x ok; VTK 9.6.2 declares ...4.0 compat).
set -euxo pipefail
python -m pip install --upgrade pip
python -m pip install "cmake>=3.22,<4.2" "ninja>=1.11" "setuptools<81" wheel
cmake --version | head -1
ninja --version
ccache --zero-stats || true
