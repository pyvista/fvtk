#!/usr/bin/env bash
#
# Regression gate: pure-Python behavior tests that exercise fvtk's C++/wrapper
# fixes (e.g. ghost-map reentrancy, GIL release). Unlike bitexact/renderexact
# these do not compare against stock VTK; they assert fvtk's own behavior on the
# built wheel. Runs on any host with a tag-compatible python — the wheel is a
# self-contained manylinux wheel, so no container is needed.
#
#   Usage: ci/run-regression.sh <wheel-dir> [base-python]   (base-python default python3)
set -euxo pipefail

WHEELDIR="${1:?usage: ci/run-regression.sh <wheel-dir> [base-python]}"
BASE_PY="${2:-python3}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# fvtk wheel in its own venv. --find-links points pip at the local dir to
# resolve `fvtk` (tag-compatible pick) while PyPI stays available for fvtk's
# declared deps — so NO --no-index. The tests import `fvtk` directly (no shim).
"$BASE_PY" -m venv /tmp/fvtk-reg
/tmp/fvtk-reg/bin/pip -q install --upgrade pip "numpy==2.4.6" pytest
/tmp/fvtk-reg/bin/pip -q install --find-links "$WHEELDIR" fvtk

cd "$SRC/tests/regression"
/tmp/fvtk-reg/bin/python -m pytest -v --tb=short -p no:cacheprovider
