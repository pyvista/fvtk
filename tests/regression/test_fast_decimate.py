#!/usr/bin/env python
"""Regression: the fvtk opt-in parallel decimation fast path (EnableFast lane).

fvtk routes vtkDecimatePro through a parallel MIS-wave half-edge-collapse kernel
(Filters/Core/fvtkFastDecimate.cxx) ONLY when fast mode is enabled
(env FVTK_FAST / fvtk.EnableFast()). The default (serial) path is byte-untouched.

The fast path is GATED on "order unimportant, points matter":
  P1 output points are an EXACT subset of input point positions (collapses copy
     existing coordinates; no point is ever created or moved), and there are
     fewer output points than input points;
  P2 the output is a valid triangle mesh (all tris, in-range indices, no
     repeated-vertex triangles);
  P3 the achieved reduction matches TargetReduction within a small tolerance
     (else the kernel discards and falls back to serial, which always hits it);
  P4 geometric fidelity to the ORIGINAL surface is within a small slack of the
     serial DecimatePro reference (both measured against the original);
  ENGAGEMENT: vtkDecimatePro.GetFastModeEngageCount() increments across the fast
     run (proving the fast path actually ran, not a serial fallback), and does
     NOT increment with fast mode off.

Needs only the built fvtk wheel (imported directly). The fast path is opt-in, so
the entire module is skipped if the build lacks the SMP fast lane (the kernel is
a no-op there and never engages).
"""
from __future__ import annotations

import os

import numpy as np
import pytest

from fvtk.vtkCommonDataModel import vtkPolyData
from fvtk.vtkFiltersCore import vtkDecimatePro, vtkTriangleFilter
from fvtk.vtkFiltersCore import vtkImplicitPolyDataDistance
from fvtk.vtkFiltersSources import vtkSphereSource, vtkPlaneSource, vtkDiskSource
from fvtk.util.numpy_support import vtk_to_numpy


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
def _tri(src):
    """Triangulate a source's output into a vtkPolyData (DecimatePro needs tris)."""
    tf = vtkTriangleFilter()
    tf.SetInputConnection(src.GetOutputPort())
    tf.Update()
    out = vtkPolyData()
    out.DeepCopy(tf.GetOutput())
    return out


def _sphere():
    s = vtkSphereSource()
    s.SetThetaResolution(80)
    s.SetPhiResolution(80)
    return _tri(s)


def _disk():
    # open surface with a real boundary loop
    d = vtkDiskSource()
    d.SetInnerRadius(0.2)
    d.SetOuterRadius(1.0)
    d.SetRadialResolution(12)
    d.SetCircumferentialResolution(48)
    return _tri(d)


def _plane():
    p = vtkPlaneSource()
    p.SetResolution(40, 40)
    return _tri(p)


def _feature_box():
    # a cube-ish surface: sharp 90-degree feature edges
    from fvtk.vtkFiltersSources import vtkCubeSource

    c = vtkCubeSource()
    return _tri(c)


def _decimate(poly, reduction, fast):
    prev = os.environ.get("FVTK_FAST")
    if fast:
        os.environ["FVTK_FAST"] = "1"
    else:
        os.environ.pop("FVTK_FAST", None)
    try:
        d = vtkDecimatePro()
        d.SetInputData(poly)
        d.SetTargetReduction(reduction)
        # The reduction-guarantee regime (and the fast-path supported regime).
        d.PreserveTopologyOff()
        d.BoundaryVertexDeletionOn()
        # MaximumError unbounded (default) -> error cap never blocks a collapse.
        d.Update()
        out = vtkPolyData()
        out.DeepCopy(d.GetOutput())
        return out
    finally:
        if prev is None:
            os.environ.pop("FVTK_FAST", None)
        else:
            os.environ["FVTK_FAST"] = prev


def _points_array(poly):
    return np.ascontiguousarray(vtk_to_numpy(poly.GetPoints().GetData()))


def _point_byte_set(poly):
    """Set of point coordinates keyed on their raw bytes (exact-equality keys)."""
    a = _points_array(poly)
    return {row.tobytes() for row in a}


def _cells_array(poly):
    polys = poly.GetPolys()
    conn = vtk_to_numpy(polys.GetConnectivityArray())
    offs = vtk_to_numpy(polys.GetOffsetsArray())
    return offs, conn


def _mean_surface_deviation(original, decimated):
    """Mean distance from the ORIGINAL surface's vertices to the DECIMATED
    surface -- i.e. how far decimation moved the geometry.

    This is the meaningful direction: the decimated output's points are an exact
    subset of the input points, so they lie ON the original surface and the
    reverse distance (decimated vertices -> original surface) is ~0 for any
    valid decimation, measuring nothing. Measuring original vertices against the
    decimated surface captures the detail the simplification dropped.

    (vtkDistancePolyDataFilter is trimmed from fvtk; vtkImplicitPolyDataDistance
    gives the point->surface distance we need.)
    """
    imp = vtkImplicitPolyDataDistance()
    imp.SetInput(decimated)
    pts = vtk_to_numpy(original.GetPoints().GetData())
    ds = np.fromiter((abs(imp.EvaluateFunction(p)) for p in pts), dtype=float, count=len(pts))
    return float(np.mean(ds))


# Skip the whole module if the fast lane never engages on this build (no SMP
# fast path compiled in). Probe once with a coarse sphere.
def _fast_lane_available():
    # AttributeError => the static accessor isn't wrapped (e.g. stock VTK or a
    # build without the fast lane): skip cleanly rather than erroring at
    # collection. On a real fvtk build the method exists and the probe must
    # engage (FastModeEnabled reads FVTK_FAST live), so a skip here on CI is a
    # red flag to investigate, not a pass.
    try:
        base = vtkDecimatePro.GetFastModeEngageCount()
    except AttributeError:
        return False
    poly = _sphere()
    _decimate(poly, 0.5, fast=True)
    return vtkDecimatePro.GetFastModeEngageCount() > base


pytestmark = pytest.mark.skipif(
    not _fast_lane_available(),
    reason="fvtk fast decimation lane not available on this build (no engagement)",
)


CASES = [
    ("sphere", _sphere, 0.5),
    ("sphere", _sphere, 0.75),
    ("disk", _disk, 0.5),
    ("plane", _plane, 0.6),
    ("box", _feature_box, 0.4),
]


# ---------------------------------------------------------------------------
# P1: output points are an EXACT subset of input point positions
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("name,maker,reduction", CASES)
def test_points_are_exact_subset(name, maker, reduction):
    poly = maker()
    in_set = _point_byte_set(poly)
    out = _decimate(poly, reduction, fast=True)

    assert out.GetNumberOfPoints() < poly.GetNumberOfPoints(), (
        f"{name}: fast decimation did not reduce point count"
    )
    out_arr = _points_array(out)
    for row in out_arr:
        assert row.tobytes() in in_set, (
            f"{name}: output point {row} is not bit-identical to any input point"
        )


# ---------------------------------------------------------------------------
# P2: valid triangle mesh
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("name,maker,reduction", CASES)
def test_valid_triangle_mesh(name, maker, reduction):
    poly = maker()
    out = _decimate(poly, reduction, fast=True)
    n = out.GetNumberOfPoints()
    offs, conn = _cells_array(out)
    sizes = np.diff(offs)
    assert np.all(sizes == 3), f"{name}: non-triangle cells present"
    assert conn.min() >= 0 and conn.max() < n, f"{name}: out-of-range vertex index"
    tris = conn.reshape(-1, 3)
    for t in tris:
        assert t[0] != t[1] and t[1] != t[2] and t[0] != t[2], (
            f"{name}: degenerate (repeated-vertex) triangle {t}"
        )


# ---------------------------------------------------------------------------
# P3: achieved reduction within tolerance of the requested reduction
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("name,maker,reduction", CASES)
def test_reduction_within_tolerance(name, maker, reduction):
    poly = maker()
    n_in = poly.GetNumberOfPolys()
    out = _decimate(poly, reduction, fast=True)
    n_out = out.GetNumberOfPolys()
    actual = 1.0 - float(n_out) / float(n_in)
    tol = max(0.02, 1.0 / n_in)
    # The fast path only KEEPS its result if it reached the target; if it had
    # fallen short it would have fallen back to serial (which also hits target).
    assert actual >= reduction - tol, (
        f"{name}: achieved reduction {actual:.4f} below target {reduction} (tol {tol:.4f})"
    )


# ---------------------------------------------------------------------------
# P4: geometric fidelity vs serial, both measured against the ORIGINAL surface
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("name,maker,reduction", CASES)
def test_fidelity_vs_serial(name, maker, reduction):
    poly = maker()
    fast = _decimate(poly, reduction, fast=True)
    ref = _decimate(poly, reduction, fast=False)

    dev_fast = _mean_surface_deviation(poly, fast)
    dev_ref = _mean_surface_deviation(poly, ref)

    slack = 0.20
    # absolute floor so near-zero serial deviation (e.g. flat plane) doesn't make
    # the relative bound impossibly tight.
    bound = dev_ref * (1.0 + slack) + 1e-6
    assert dev_fast <= bound, (
        f"{name}: fast deviation {dev_fast:.6g} exceeds serial {dev_ref:.6g} "
        f"* (1+{slack}) (+floor) = {bound:.6g}"
    )


# ---------------------------------------------------------------------------
# ENGAGEMENT: the counter proves the fast path actually ran
# ---------------------------------------------------------------------------
def test_engagement_counter_increments_with_fast_on():
    poly = _sphere()
    before = vtkDecimatePro.GetFastModeEngageCount()
    out = _decimate(poly, 0.5, fast=True)
    after = vtkDecimatePro.GetFastModeEngageCount()
    assert after > before, "fast path did not engage with FVTK_FAST=1"
    assert out.GetNumberOfPoints() < poly.GetNumberOfPoints()


def test_engagement_counter_stable_with_fast_off():
    poly = _sphere()
    before = vtkDecimatePro.GetFastModeEngageCount()
    _decimate(poly, 0.5, fast=False)
    after = vtkDecimatePro.GetFastModeEngageCount()
    assert after == before, "fast path engaged even though FVTK_FAST was unset"


def test_fast_and_serial_pointsets_differ():
    """A secondary engagement proof: the fast (order-relaxed) point set is not
    the same renumbering as serial, yet both are valid subset decimations."""
    poly = _sphere()
    fast = _decimate(poly, 0.6, fast=True)
    ref = _decimate(poly, 0.6, fast=False)
    # Both subset the input; the chosen subsets / order generally differ.
    fa = _points_array(fast)
    ra = _points_array(ref)
    # Either a different count or a different ordering proves a distinct path.
    assert fa.shape != ra.shape or not np.array_equal(fa, ra)
