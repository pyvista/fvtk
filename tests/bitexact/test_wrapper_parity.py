"""Pytest gate for wrapper-behavior parity (abi3 migration safety net).

Runs wrapper_parity.py under the stock-VTK and cvista pythons (the same two
backends the numeric suite uses) and asserts the captured wrapper-behavior
facts — type identity/flags, isinstance/mro, repr format, numpy zero-copy
buffer protocol, weakref, instance __dict__ — are identical.

Skips cleanly (like the numeric suite) when the two backend pythons are not
configured via BITEXACT_STOCK_PY / BITEXACT_CVISTA_PY.
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
def parity_dirs(tmp_path_factory):
    base = os.environ.get("BITEXACT_OUTDIR") or str(
        tmp_path_factory.mktemp("parity")
    )
    stock_dir = os.path.join(base, "parity_stock")
    cvista_dir = os.path.join(base, "parity_cvista")
    _run_probe(os.environ.get("BITEXACT_STOCK_PY"),
               os.environ.get("BITEXACT_STOCK_LDLP", ""), stock_dir, "STOCK")
    _run_probe(os.environ.get("BITEXACT_CVISTA_PY"),
               os.environ.get("BITEXACT_CVISTA_LDLP", ""), cvista_dir, "CVISTA")
    return stock_dir, cvista_dir


@pytest.fixture(scope="session")
def parity_mismatches(parity_dirs):
    return _wp.compare_parity(*parity_dirs)


def test_wrapper_behavior_parity(parity_mismatches):
    assert parity_mismatches == [], (
        "wrapper-behavior parity broken vs stock VTK:\n"
        + "\n".join(f"  {k}: stock={s!r} cvista={f!r}" for k, s, f in parity_mismatches)
    )


@pytest.mark.skipif(not _wp._is_abi3(), reason="abi3 build not under test (BITEXACT_ABI3 unset)")
def test_abi3_heaptypes_in_effect(parity_dirs):
    """Under abi3 the port must ACTUALLY be in effect: every probed wrapped type
    and the reference helper type must report Py_TPFLAGS_HEAPTYPE set (and
    IMMUTABLETYPE cleared) on the cvista side. If any stays a static type the heap
    conversion silently didn't happen for it — fail loudly. This is the positive
    counterpart to compare_parity's tolerance of the flag flip."""
    import json
    with open(os.path.join(parity_dirs[1], "parity.json")) as f:
        fv = json.load(f)
    heap_keys = [k for k in fv if "flag_heaptype" in k]
    assert heap_keys, "probe produced no flag_heaptype facts"
    not_heap = [k for k in heap_keys if fv[k] is not True]
    assert not_heap == [], f"abi3 build: these types are NOT heap types: {not_heap}"
    immut = [k for k in fv if "flag_immutabletype" in k and fv[k] is not False]
    assert immut == [], f"abi3 build: these types are still IMMUTABLETYPE: {immut}"
