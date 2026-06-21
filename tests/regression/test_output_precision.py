#!/usr/bin/env python
"""Regression: filters preserve input point precision by default.

A VTK filter that takes a point-based input and emits output points must, by
DEFAULT (vtkAlgorithm::DEFAULT_PRECISION), produce output points of the SAME
data type as the input points; SetOutputPointsPrecision(SINGLE/DOUBLE) forces
float/double. Stock VTK 9.6.2 has a long-standing bug where ~30 filters silently
downcast float64 -> float32 (and a couple force float -> double); fvtk fixes
them by adding the standard OutputPointsPrecision API. This test asserts the
corrected behavior on the built fvtk wheel (no stock comparison).

Two layers:
  * API presence: every fixed filter exposes Get/SetOutputPointsPrecision and
    defaults to DEFAULT_PRECISION (0).
  * Behavior: for representative filters spanning every input category
    (polydata, polyline, unstructured grid of tets, structured grid), a float32
    input yields float32 output and a float64 input yields float64 output under
    DEFAULT; SINGLE/DOUBLE override regardless of input.
"""

import numpy as np
import pytest

from fvtk.vtkCommonCore import vtkPoints, VTK_FLOAT, VTK_DOUBLE
from fvtk.vtkCommonDataModel import vtkPolyData, vtkStructuredGrid, vtkUnstructuredGrid, vtkCellArray
from fvtk.vtkFiltersSources import vtkSphereSource
from fvtk.vtkFiltersCore import vtkTriangleFilter, vtkDelaunay3D
from fvtk.util.numpy_support import numpy_to_vtk, vtk_to_numpy

# vtkAlgorithm::DesiredOutputPrecision enum values (order is SINGLE, DOUBLE,
# DEFAULT -- so DEFAULT is 2, NOT 0). DEFAULT means "match the input precision".
SINGLE_PRECISION = 0
DOUBLE_PRECISION = 1
DEFAULT_PRECISION = 2
VTK_DTYPE = {np.float32: VTK_FLOAT, np.float64: VTK_DOUBLE}


# ---------------------------------------------------------------------------
# (module, classname) for every filter that received the OutputPointsPrecision
# fix. Abstract bases are represented by a concrete subclass that inherits it.
# ---------------------------------------------------------------------------
FIXED_FILTERS = [
    ("vtkFiltersCore", "vtkBinnedDecimation"),
    ("vtkFiltersCore", "vtkQuadricClustering"),
    ("vtkFiltersCore", "vtkStructuredGridOutlineFilter"),
    ("vtkFiltersExtraction", "vtkExtractCellsByType"),
    ("vtkFiltersExtraction", "vtkExtractPolyDataGeometry"),
    ("vtkFiltersExtraction", "vtkExtractUnstructuredGrid"),
    # vtkAxisAlignedReflectionFilter, vtkClipConvexPolyData,
    # vtkUnstructuredGridGeometryFilter, vtkWeightedTransformFilter received the
    # same C++ OutputPointsPrecision fix but are NOT Python-wrapped in fvtk, so
    # they cannot be exercised here -- their fix is covered by compilation and
    # tests/precision_audit.md. (Confirmed not present in any fvtk.* module.)
    ("vtkFiltersGeneral", "vtkBooleanOperationPolyDataFilter"),
    ("vtkFiltersGeneral", "vtkBoxClipDataSet"),
    ("vtkFiltersGeneral", "vtkDataSetTriangleFilter"),
    ("vtkFiltersGeneral", "vtkExtractSelectedFrustum"),
    ("vtkFiltersGeneral", "vtkIntersectionPolyDataFilter"),
    ("vtkFiltersGeneral", "vtkShrinkFilter"),
    ("vtkFiltersGeneral", "vtkSplitByCellScalarFilter"),
    ("vtkFiltersGeneral", "vtkTessellatorFilter"),
    # vtkApproximatingSubdivisionFilter is abstract; a concrete subclass inherits the ivar.
    ("vtkFiltersModeling", "vtkLoopSubdivisionFilter"),
    ("vtkFiltersGeometry", "vtkStructuredGridGeometryFilter"),
    ("vtkFiltersHybrid", "vtkProjectedTerrainPath"),
    ("vtkFiltersModeling", "vtkBandedPolyDataContourFilter"),
    ("vtkFiltersModeling", "vtkDijkstraGraphGeodesicPath"),
    ("vtkFiltersModeling", "vtkLinearExtrusionFilter"),
    ("vtkFiltersModeling", "vtkRibbonFilter"),
    ("vtkFiltersModeling", "vtkRotationalExtrusionFilter"),
    ("vtkFiltersModeling", "vtkRuledSurfaceFilter"),
    ("vtkFiltersModeling", "vtkSubdivideTetra"),
    ("vtkFiltersParallel", "vtkRectilinearGridOutlineFilter"),
]


def _load(module, classname):
    mod = __import__(f"fvtk.{module}", fromlist=[classname])
    return getattr(mod, classname)


@pytest.mark.parametrize("module,classname", FIXED_FILTERS, ids=[c for _, c in FIXED_FILTERS])
def test_output_points_precision_api(module, classname):
    """Every fixed filter exposes the API and defaults to DEFAULT_PRECISION."""
    cls = _load(module, classname)
    obj = cls()
    assert hasattr(obj, "GetOutputPointsPrecision"), f"{classname} missing GetOutputPointsPrecision"
    assert hasattr(obj, "SetOutputPointsPrecision"), f"{classname} missing SetOutputPointsPrecision"
    assert obj.GetOutputPointsPrecision() == DEFAULT_PRECISION, (
        f"{classname} default precision is {obj.GetOutputPointsPrecision()}, expected DEFAULT(0)")
    # The setter round-trips.
    obj.SetOutputPointsPrecision(DOUBLE_PRECISION)
    assert obj.GetOutputPointsPrecision() == DOUBLE_PRECISION


# ---------------------------------------------------------------------------
# Input builders, each parameterized by numpy point dtype.
# ---------------------------------------------------------------------------
def _retype_points(ds, dtype):
    """Return ds with its points re-stored in the given numpy dtype (values kept)."""
    arr = vtk_to_numpy(ds.GetPoints().GetData()).astype(dtype)
    pts = vtkPoints()
    pts.SetData(numpy_to_vtk(np.ascontiguousarray(arr), deep=1))
    ds.SetPoints(pts)
    return ds


def _sphere_polydata(dtype):
    s = vtkSphereSource()
    s.SetThetaResolution(8)
    s.SetPhiResolution(8)
    s.Update()
    t = vtkTriangleFilter()
    t.SetInputData(s.GetOutput())
    t.Update()
    pd = vtkPolyData()
    pd.DeepCopy(t.GetOutput())
    return _retype_points(pd, dtype)


def _polyline(dtype):
    n = 12
    coords = np.zeros((n, 3))
    coords[:, 0] = np.linspace(-1.0, 1.0, n)
    coords[:, 1] = np.sin(coords[:, 0])
    pd = vtkPolyData()
    pts = vtkPoints()
    pts.SetData(numpy_to_vtk(np.ascontiguousarray(coords.astype(dtype)), deep=1))
    pd.SetPoints(pts)
    lines = vtkCellArray()
    lines.InsertNextCell(n)
    for i in range(n):
        lines.InsertCellPoint(i)
    pd.SetLines(lines)
    return pd


def _ug_tets(dtype):
    s = vtkSphereSource()
    s.SetThetaResolution(8)
    s.SetPhiResolution(8)
    s.Update()
    d = vtkDelaunay3D()
    d.SetInputData(s.GetOutput())
    d.Update()
    ug = vtkUnstructuredGrid()
    ug.DeepCopy(d.GetOutput())
    return _retype_points(ug, dtype)


def _structured_grid(dtype):
    nx, ny, nz = 5, 4, 3
    xs, ys, zs = np.meshgrid(
        np.linspace(0, 1, nx), np.linspace(0, 1, ny), np.linspace(0, 1, nz), indexing="ij")
    coords = np.stack([xs.ravel(order="F"), ys.ravel(order="F"), zs.ravel(order="F")], axis=1)
    sg = vtkStructuredGrid()
    sg.SetDimensions(nx, ny, nz)
    pts = vtkPoints()
    pts.SetData(numpy_to_vtk(np.ascontiguousarray(coords.astype(dtype)), deep=1))
    sg.SetPoints(pts)
    return sg


def _run(filt, ds):
    filt.SetInputData(ds)
    filt.Update()
    return filt.GetOutput()


# (classname, module, input-builder, filter-configurator) for behavioral checks.
def _cfg_noop(f):
    return f


def _cfg_extrude(f):
    f.SetExtrusionTypeToVectorExtrusion()
    f.SetVector(0, 0, 1)
    return f


def _cfg_split(f):
    # needs a cell-data scalar to split on
    return f


BEHAVIORAL = [
    ("vtkFiltersGeneral", "vtkShrinkFilter", _sphere_polydata, _cfg_noop),
    ("vtkFiltersModeling", "vtkLinearExtrusionFilter", _polyline, _cfg_extrude),
    ("vtkFiltersModeling", "vtkRotationalExtrusionFilter", _polyline, _cfg_noop),
    ("vtkFiltersModeling", "vtkRibbonFilter", _polyline, _cfg_noop),
    ("vtkFiltersModeling", "vtkSubdivideTetra", _ug_tets, _cfg_noop),
    ("vtkFiltersGeometry", "vtkStructuredGridGeometryFilter", _structured_grid, _cfg_noop),
    ("vtkFiltersCore", "vtkStructuredGridOutlineFilter", _structured_grid, _cfg_noop),
]


@pytest.mark.parametrize("module,classname,builder,cfg", BEHAVIORAL,
                         ids=[c for _, c, _, _ in BEHAVIORAL])
@pytest.mark.parametrize("dtype", [np.float32, np.float64], ids=["f32", "f64"])
def test_default_precision_matches_input(module, classname, builder, cfg, dtype):
    """DEFAULT_PRECISION: output point dtype follows the input point dtype."""
    cls = _load(module, classname)
    ds = builder(dtype)
    assert ds.GetPoints().GetDataType() == VTK_DTYPE[dtype]  # input is what we think
    out = _run(cfg(cls()), ds)
    assert out is not None and out.GetNumberOfPoints() > 0, f"{classname} produced no points"
    assert out.GetPoints().GetDataType() == VTK_DTYPE[dtype], (
        f"{classname}: float input dtype {VTK_DTYPE[dtype]} -> output "
        f"{out.GetPoints().GetDataType()} (precision not preserved)")


@pytest.mark.parametrize("module,classname,builder,cfg", BEHAVIORAL,
                         ids=[c for _, c, _, _ in BEHAVIORAL])
def test_forced_precision_overrides_input(module, classname, builder, cfg):
    """SINGLE/DOUBLE force the output dtype regardless of input dtype."""
    cls = _load(module, classname)
    # float32 input forced to double
    f = cfg(cls())
    f.SetOutputPointsPrecision(DOUBLE_PRECISION)
    out = _run(f, builder(np.float32))
    assert out.GetPoints().GetDataType() == VTK_DOUBLE, f"{classname}: SetDOUBLE on f32 input"
    # float64 input forced to single
    f = cfg(cls())
    f.SetOutputPointsPrecision(SINGLE_PRECISION)
    out = _run(f, builder(np.float64))
    assert out.GetPoints().GetDataType() == VTK_FLOAT, f"{classname}: SetSINGLE on f64 input"
