"""Pytest gate for wrapper-behavior parity (abi3 migration safety net).

Runs wrapper_parity.py under the stock-VTK and fvtk pythons (the same two
backends the numeric suite uses) and asserts the captured wrapper-behavior
facts — type identity/flags, isinstance/mro, repr format, numpy zero-copy
buffer protocol, weakref, instance __dict__ — are identical.

Skips cleanly (like the numeric suite) when the two backend pythons are not
configured via BITEXACT_STOCK_PY / BITEXACT_FVTK_PY.
"""
from __future__ import annotations

import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, "wrapper_parity.py")

sys.path.insert(0, HERE)
import wrapper_parity as _wp  # noqa: E402


def _env_with_ldlp(ldlp):
    env = dict(os.environ)
    if ldlp:
        existing = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = ldlp + (":" + existing if existing else "")
    return env


def _run_probe(py, ldlp, outdir, label):
    if not py:
        pytest.skip(f"BITEXACT_{label}_PY not set; cannot run wrapper-parity probe.")
    os.makedirs(outdir, exist_ok=True)
    proc = subprocess.run(
        [py, PROBE, outdir],
        env=_env_with_ldlp(ldlp),
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"{label} wrapper-parity probe failed (rc={proc.returncode}):\n"
            f"STDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}"
        )
    return proc.stdout


@pytest.fixture(scope="session")
def parity_mismatches(tmp_path_factory):
    base = os.environ.get("BITEXACT_OUTDIR") or str(
        tmp_path_factory.mktemp("parity")
    )
    stock_dir = os.path.join(base, "parity_stock")
    fvtk_dir = os.path.join(base, "parity_fvtk")
    _run_probe(os.environ.get("BITEXACT_STOCK_PY"),
               os.environ.get("BITEXACT_STOCK_LDLP", ""), stock_dir, "STOCK")
    _run_probe(os.environ.get("BITEXACT_FVTK_PY"),
               os.environ.get("BITEXACT_FVTK_LDLP", ""), fvtk_dir, "FVTK")
    return _wp.compare_parity(stock_dir, fvtk_dir)


def test_wrapper_behavior_parity(parity_mismatches):
    assert parity_mismatches == [], (
        "wrapper-behavior parity broken vs stock VTK:\n"
        + "\n".join(f"  {k}: stock={s!r} fvtk={f!r}" for k, s, f in parity_mismatches)
    )
