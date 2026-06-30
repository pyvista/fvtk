"""Bit-exactness regression tests: cvista (this fork) vs stock VTK 9.6.2.

Each test asserts that one operation, run under both backends from byte-identical
inputs, produces byte-identical output across EVERY array (points, all point/cell
data arrays, and topology/connectivity). Tolerance is exact: maxULP == 0.

Ops corresponding to the 9 modified filters carry the ``modified`` marker and are
the hard gate — run them alone with ``-m modified``.

Prereqs (see conftest.py): BITEXACT_STOCK_PY / BITEXACT_CVISTA_PY pointing at a
stock-VTK python and an cvista python; tests skip cleanly if unset.
"""
from __future__ import annotations

import sys
import os

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ops as _ops  # noqa: E402


def test_provenance_inputs_identical(results):
    """Gate: both backends ran the SAME numpy and started from byte-identical
    inputs. If this fails, any downstream diff is an input artifact, not cvista."""
    prov = results["provenance"]
    assert prov["numpy_match"], (
        f"numpy version mismatch stock={prov['numpy_stock']} "
        f"cvista={prov['numpy_cvista']}; pin numpy==2.4.6 on both venvs"
    )
    assert prov["inputs_digest_match"], (
        "input array digests differ between backends — inputs are NOT "
        f"bit-identical: stock={prov['inputs_digest_f64_stock'][:16]} "
        f"cvista={prov['inputs_digest_f64_cvista'][:16]}"
    )
    assert prov["vtk_stock"] == "9.6.2", prov["vtk_stock"]
    assert prov["vtk_cvista"] == "9.6.2", prov["vtk_cvista"]


def _assert_case(results, case_key):
    case = results["cases"].get(case_key)
    assert case is not None, f"{case_key} missing from comparison"
    if not case["ok"]:
        detail = case["detail"]
        # Build a focused failure message listing the non-equal arrays + ULP.
        msg = [f"BIT DIFFERENCE in {case_key}:"]
        if detail.get("order_relaxed"):
            msg.append("  (order-relaxed mesh comparison)")
        if detail.get("corrects_stock"):
            msg.append(
                f"  (divergence ledger: cvista corrects stock precision bug; "
                f"input_dtype={detail.get('input_dtype')})")
        if "arrays" in detail:
            for name, info in detail["arrays"].items():
                if not info.get("equal", True):
                    msg.append(
                        f"  array {name}: equal=False mode={info.get('mode', 'strict')} "
                        f"dtype={info.get('dtype', '?')} "
                        f"shape_stock={info.get('shape_stock', '?')} "
                        f"shape_cvista={info.get('shape_cvista', '?')} "
                        f"ulp={info.get('ulp')} reason={info.get('reason', '')}"
                    )
        else:
            msg.append(f"  {detail}")
        pytest.fail("\n".join(msg))


@pytest.mark.bitexact
def test_bitexact(results, case_key, op_name):
    """Every output array byte-identical between cvista and stock VTK."""
    _assert_case(results, case_key)


def test_modified_filters_are_covered():
    """Guard: all 9 modified filters are present in the registry and marked."""
    expected = {
        "decimate", "smooth", "normals", "contour", "clip",
        "threshold", "warp", "glyph", "cell2point",
    }
    assert _ops.MODIFIED_OPS == expected, (
        f"modified-filter set drifted: {_ops.MODIFIED_OPS} != {expected}"
    )
