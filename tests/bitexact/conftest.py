"""Pytest fixtures for the cvista bit-exactness suite.

The suite runs ``run_ops.py`` once under a stock-VTK python and once under an
cvista python, then compares the two output dirs. Those two pythons are configured
entirely through environment variables so the SAME suite runs unchanged locally
and in CI:

  BITEXACT_STOCK_PY   path to a python whose ``vtkmodules`` is stock VTK 9.6.2
  BITEXACT_CVISTA_PY    path to a python whose ``vtkmodules`` resolves to cvista
                      (via the _cvista_shim / sitecustomize redirect)
  BITEXACT_STOCK_LDLP optional LD_LIBRARY_PATH for the stock python
  BITEXACT_CVISTA_LDLP  optional LD_LIBRARY_PATH for the cvista python
  BITEXACT_OUTDIR     optional dir for the dumped npz/manifests (default: tmp)

On a manylinux/ubuntu CI runner stock vtk+numpy are self-contained, so
BITEXACT_STOCK_LDLP is usually empty; only the cvista wheel needs the nix runtime
libs (libstdc++, libz) on its loader path — exactly the pattern the smoke job in
ci.yml already uses.

The two backend runs happen ONCE per session (a session-scoped fixture); the
parametrized tests then just read the already-dumped arrays. This keeps the run
fast and makes every per-case assertion independent in the report.
"""
from __future__ import annotations

import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
RUN_OPS = os.path.join(HERE, "run_ops.py")

sys.path.insert(0, HERE)
import compare as _compare  # noqa: E402
import ops as _ops  # noqa: E402


def _env_with_ldlp(ldlp):
    env = dict(os.environ)
    if ldlp:
        existing = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = ldlp + (":" + existing if existing else "")
    return env


def _run_backend(py, ldlp, outdir, label):
    if not py:
        pytest.skip(
            f"BITEXACT_{label}_PY not set; cannot run {label} backend. "
            "Set BITEXACT_STOCK_PY and BITEXACT_CVISTA_PY to the two pythons."
        )
    os.makedirs(outdir, exist_ok=True)
    proc = subprocess.run(
        [py, RUN_OPS, outdir],
        env=_env_with_ldlp(ldlp),
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"{label} backend run failed (rc={proc.returncode}):\n"
            f"STDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}"
        )
    return proc.stdout


@pytest.fixture(scope="session")
def results(tmp_path_factory):
    base = os.environ.get("BITEXACT_OUTDIR") or str(
        tmp_path_factory.mktemp("bitexact")
    )
    stock_dir = os.path.join(base, "stock")
    cvista_dir = os.path.join(base, "cvista")

    stock_py = os.environ.get("BITEXACT_STOCK_PY")
    cvista_py = os.environ.get("BITEXACT_CVISTA_PY")
    stock_ldlp = os.environ.get("BITEXACT_STOCK_LDLP", "")
    cvista_ldlp = os.environ.get("BITEXACT_CVISTA_LDLP", "")

    out_stock = _run_backend(stock_py, stock_ldlp, stock_dir, "STOCK")
    out_cvista = _run_backend(cvista_py, cvista_ldlp, cvista_dir, "CVISTA")
    print("\n" + out_stock + out_cvista)

    res = _compare.compare_all(stock_dir, cvista_dir)
    res["_dirs"] = {"stock": stock_dir, "cvista": cvista_dir}
    return res


def pytest_generate_tests(metafunc):
    if "case_key" in metafunc.fixturenames:
        ids = []
        params = []
        for op_name, dtype_name, size in _ops.iter_cases():
            key = f"{op_name}__{dtype_name}__{size}"
            marks = []
            if op_name in _ops.MODIFIED_OPS:
                marks.append(pytest.mark.modified)
            params.append(pytest.param(key, op_name, marks=marks))
            ids.append(key)
        metafunc.parametrize("case_key,op_name", params, ids=ids)
