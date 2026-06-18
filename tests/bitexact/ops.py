"""Bit-exactness operation registry for the fvtk vs stock-VTK regression suite.

Every operation here is written against the *vtkmodules* API only (no pyvista, no
``import vtk``). That is the load-bearing property of this suite: the exact same
source drives two backends —

  * stock VTK 9.6.2   — ``vtkmodules`` resolves to the upstream wheel
  * fvtk (this fork)  — the ``_fvtk_shim`` redirects ``vtkmodules.*`` -> ``fvtk.*``

so the *only* thing that differs between the two runs is the compiled C++
backend. Any byte difference in an output array is therefore attributable to
fvtk's build, not to Python-level nondeterminism.

Determinism rules (so inputs are bit-identical on both sides):
  * numpy is pinned to the SAME version on both venvs (numpy==2.4.6).
  * Inputs are built ONLY from deterministic integer/linspace/arange ops and
    pure-algebra. We deliberately avoid ``np.sin``/``np.cos`` on the inputs,
    whose last-ULP results can drift across numpy/libm builds, which would
    masquerade as an fvtk divergence. ``build_inputs_digest()`` hashes every
    constructed input array so the harness can *prove* the two sides started
    from identical bytes before blaming the filter.

Each operation is a callable ``fn(dtype, size) -> vtkDataObject | dict``:
  * returning a vtkDataObject -> captured via ``capture_dataobject``
  * returning a dict of name->np.ndarray -> captured verbatim (used for the
    vtkCommon / math / locator ops that don't produce a dataset)

The registry is grouped so the pytest layer can mark the 9 modified filters as a
hard gate while still exercising broad coverage.
"""
from __future__ import annotations

import hashlib
import os
import tempfile

import numpy as np

# --- vtkmodules imports (resolve to stock vtk OR fvtk depending on the venv) ---
# Guarded so this module imports cleanly on a runner python that has numpy but no
# VTK (the pytest *driver* only needs the registry metadata — iter_cases,
# MODIFIED_OPS — to parametrize; the actual op bodies run in the two backend
# subprocesses via run_ops.py, each of which DOES have its VTK). If an op body is
# called without VTK present, it fails loudly via _require_vtk().
try:
    from vtkmodules.vtkCommonCore import (
        reference,
        vtkDoubleArray,
        vtkFloatArray,
        vtkIdList,
        vtkMath,
        vtkPoints,
    )
    from vtkmodules.vtkCommonDataModel import (
        vtkCellArray,
        vtkCellLocator,
        vtkDataObject,
        vtkGenericCell,
        vtkImageData,
        vtkMergePoints,
        vtkPlane,
        vtkPointLocator,
        vtkPolyData,
        vtkStaticPointLocator,
        vtkStructuredGrid,
        vtkUnstructuredGrid,
    )
    from vtkmodules.vtkFiltersExtraction import vtkExtractGrid
    from vtkmodules.vtkFiltersCore import (
        vtkCellCenters,
        vtkCellDataToPointData,
        vtkPointDataToCellData,
        vtkContourFilter,
        vtkFeatureEdges,
        vtkGlyph3D,
        vtkPolyDataNormals,
        vtkQuadricDecimation,
        vtkSimpleElevationFilter,
        vtkSmoothPolyDataFilter,
        vtkStripper,
        vtkThreshold,
        vtkTriangleFilter,
        vtkAppendFilter,
        vtkAppendPolyData,
        vtkCleanPolyData,
        vtkConnectivityFilter,
        vtkContourGrid,
        vtkCutter,
        vtkDecimatePro,
        vtkElevationFilter,
        vtkProbeFilter,
        vtkTubeFilter,
    )
    from vtkmodules.vtkFiltersGeneral import (
        vtkClipDataSet,
        vtkContourTriangulator,
        vtkDataSetTriangleFilter,
        vtkGradientFilter,
        vtkShrinkFilter,
        vtkTableBasedClipDataSet,
        vtkTransformFilter,
        vtkVertexGlyphFilter,
        vtkWarpScalar,
        vtkWarpVector,
    )
    from vtkmodules.vtkCommonTransforms import (
        vtkPerspectiveTransform,
        vtkThinPlateSplineTransform,
        vtkTransform,
    )
    from vtkmodules.vtkFiltersGeneral import vtkTransformPolyDataFilter
    from vtkmodules.vtkIOPLY import (
        vtkPLYReader,
        vtkPLYWriter,
    )
    from vtkmodules.vtkFiltersGeometry import (
        vtkDataSetSurfaceFilter,
        vtkGeometryFilter,
    )
    from vtkmodules.vtkFiltersSources import (
        vtkArrowSource,
        vtkConeSource,
        vtkSphereSource,
    )
    from vtkmodules.util.numpy_support import numpy_to_vtk, vtk_to_numpy

    _HAVE_VTK = True
    _VTK_IMPORT_ERROR = None
except Exception as _e:  # noqa: BLE001
    _HAVE_VTK = False
    _VTK_IMPORT_ERROR = _e


def _require_vtk():
    if not _HAVE_VTK:
        raise RuntimeError(
            "vtkmodules (stock VTK or fvtk) is not importable in this "
            f"interpreter: {_VTK_IMPORT_ERROR!r}. Op bodies must run under a "
            "backend venv via run_ops.py."
        )


# Dtype name -> numpy dtype. Sizes are mesh-resolution knobs interpreted per op.
DTYPES = {"float32": np.float32, "float64": np.float64}


# ---------------------------------------------------------------------------
# Deterministic input builders. No transcendental ops on the data path.
# ---------------------------------------------------------------------------
def _radial_field(n, dtype):
    """||index - center|| over an n^3 grid, raveled in VTK point order (x fastest).

    Pure integer index arithmetic + a single sqrt -> reproducible to the ULP
    across numpy builds (sqrt is correctly-rounded per IEEE-754, unlike sin/cos).
    """
    idx = np.indices((n, n, n), dtype=np.float64)
    field = np.sqrt(((idx - (n - 1) / 2.0) ** 2).sum(axis=0))
    flat = np.ascontiguousarray(field.transpose(2, 1, 0).ravel())
    return flat.astype(dtype)


def _ramp_field_2d(n, dtype):
    """A smooth-ish bilinear-ish ramp over an n x n grid (no trig)."""
    gx = np.linspace(-1.0, 1.0, n, dtype=np.float64)
    xv, yv = np.meshgrid(gx, gx)
    # quartic bump: deterministic, varied curvature, no libm transcendentals
    field = (1.0 - xv * xv) * (1.0 - yv * yv) + 0.25 * xv * yv
    flat = np.ascontiguousarray(field.T.ravel())
    return flat.astype(dtype)


def make_volume(n=24, dtype=np.float64):
    img = vtkImageData()
    img.SetDimensions(n, n, n)
    arr = numpy_to_vtk(_radial_field(n, dtype), deep=1)
    arr.SetName("v")
    img.GetPointData().SetScalars(arr)
    return img


def make_grid2d(n=64, dtype=np.float64):
    img = vtkImageData()
    img.SetDimensions(n, n, 1)
    arr = numpy_to_vtk(_ramp_field_2d(n, dtype), deep=1)
    arr.SetName("s")
    img.GetPointData().SetScalars(arr)
    return img


def make_sphere(theta=40, phi=40):
    s = vtkSphereSource()
    s.SetThetaResolution(theta)
    s.SetPhiResolution(phi)
    t = vtkTriangleFilter()
    t.SetInputConnection(s.GetOutputPort())
    t.Update()
    return t.GetOutput()


def make_sphere_with_vectors(theta=40, phi=40, dtype=np.float64):
    """Sphere whose point normals are copied into a 3-component 'vec' array so
    warp-vector / glyph-by-vector have a deterministic vector field."""
    s = make_sphere(theta, phi)
    n = vtkPolyDataNormals()
    n.SetInputData(s)
    n.SetComputePointNormals(True)
    n.Update()
    out = n.GetOutput()
    normals = vtk_to_numpy(out.GetPointData().GetNormals()).astype(dtype)
    va = numpy_to_vtk(np.ascontiguousarray(normals), deep=1)
    va.SetName("vec")
    out.GetPointData().AddArray(va)
    out.GetPointData().SetVectors(va)
    return out


def make_points_array(n=2000, dtype=np.float64):
    """A deterministic point cloud (lattice-ish, no trig)."""
    k = int(round(n ** (1.0 / 3.0))) + 1
    lin = np.linspace(0.0, 1.0, k, dtype=np.float64)
    gx, gy, gz = np.meshgrid(lin, lin, lin, indexing="ij")
    pts = np.stack([gx.ravel(), gy.ravel(), gz.ravel()], axis=1)[:n]
    return np.ascontiguousarray(pts).astype(dtype)


def make_polylines(nlines=6, length=20, dtype=np.float64):
    """A vtkPolyData of `nlines` open polylines, each `length` points, with a
    deterministic per-point scalar 's'. Coordinates and scalars use only integer
    algebra (no trig) so both backends start byte-identical. Used by the tube
    filter, which interpolates the scalar onto every generated tube vertex."""
    npts = nlines * length
    idx = np.arange(npts, dtype=np.int64)
    li = idx // length  # which polyline
    pi = idx % length  # position along the polyline
    coords = np.empty((npts, 3), dtype=dtype)
    coords[:, 0] = pi.astype(dtype)
    coords[:, 1] = (li + (pi % 5)).astype(dtype)  # deterministic zigzag
    coords[:, 2] = (pi % 3).astype(dtype)
    pd = vtkPolyData()
    vp = vtkPoints()
    vp.SetData(numpy_to_vtk(np.ascontiguousarray(coords), deep=1))
    pd.SetPoints(vp)
    lines = vtkCellArray()
    for L in range(nlines):
        ids = vtkIdList()
        for p in range(length):
            ids.InsertNextId(L * length + p)
        lines.InsertNextCell(ids)
    pd.SetLines(lines)
    scal = numpy_to_vtk(np.ascontiguousarray((1.0 + (pi % 4)).astype(dtype)), deep=1)
    scal.SetName("s")
    pd.GetPointData().SetScalars(scal)
    return pd


def make_hex_ugrid(n=10, dtype=np.float64):
    """A vtkUnstructuredGrid of hexahedra on an n*n*n integer lattice, with a
    deterministic radial point scalar 'v'. 3D cells so a plane cut with
    GenerateTrianglesOff exercises the vtkContourHelper 3D-cell merge path."""
    from vtkmodules.vtkCommonDataModel import VTK_HEXAHEDRON

    lin = np.arange(n, dtype=dtype)
    gx, gy, gz = np.meshgrid(lin, lin, lin, indexing="ij")
    pts = np.ascontiguousarray(
        np.stack([gx.ravel(), gy.ravel(), gz.ravel()], axis=1)
    )
    vp = vtkPoints()
    vp.SetData(numpy_to_vtk(pts, deep=1))
    ug = vtkUnstructuredGrid()
    ug.SetPoints(vp)

    def pid(i, j, k):
        return i + n * (j + n * k)

    ncells = (n - 1) ** 3
    ug.Allocate(ncells)
    ids = vtkIdList()
    for k in range(n - 1):
        for j in range(n - 1):
            for i in range(n - 1):
                ids.Reset()
                for di, dj in ((0, 0), (1, 0), (1, 1), (0, 1)):
                    ids.InsertNextId(pid(i + di, j + dj, k))
                for di, dj in ((0, 0), (1, 0), (1, 1), (0, 1)):
                    ids.InsertNextId(pid(i + di, j + dj, k + 1))
                ug.InsertNextCell(VTK_HEXAHEDRON, ids)
    # radial point scalar (integer algebra; matches make_volume's spirit, no trig)
    c = (n - 1) / 2.0
    field = ((pts[:, 0] - c) ** 2 + (pts[:, 1] - c) ** 2 + (pts[:, 2] - c) ** 2).astype(
        dtype
    )
    arr = numpy_to_vtk(np.ascontiguousarray(field), deep=1)
    arr.SetName("v")
    ug.GetPointData().SetScalars(arr)
    return ug


def make_tet_ugrid(n=8, dtype=np.float64):
    """Tetrahedralize the hex grid deterministically (vtkDataSetTriangleFilter)
    so clip/contour exercise vtkTetra::Clip / vtkTetra::Contour. The tessellation
    runs in the C++ backend identically on both sides, so it is a fair input."""
    t = vtkDataSetTriangleFilter()
    t.SetInputData(make_hex_ugrid(n, dtype))
    t.Update()
    return t.GetOutput()


def make_wedge_pyramid_ugrid(nrep=3, dtype=np.float64):
    """A vtkUnstructuredGrid of explicit WEDGE + PYRAMID cells with a per-point
    z-coordinate scalar (integer/half-integer coords -> deterministic, no trig).
    Lets vtkContourGrid drive vtkWedge::Contour / vtkPyramid::Contour per cell."""
    from vtkmodules.vtkCommonDataModel import VTK_WEDGE, VTK_PYRAMID

    pts = []
    cells = []  # (celltype, [point ids])

    def add(coords):
        base = len(pts)
        pts.extend(coords)
        return base

    for r in range(nrep):
        z = 2.0 * r
        b = add([(0, 0, z), (2, 0, z), (0, 2, z),
                 (0, 0, z + 1), (2, 0, z + 1), (0, 2, z + 1)])
        cells.append((VTK_WEDGE, [b, b + 1, b + 2, b + 3, b + 4, b + 5]))
        b2 = add([(3, 0, z), (5, 0, z), (5, 2, z), (3, 2, z), (4, 1, z + 1.5)])
        cells.append((VTK_PYRAMID, [b2, b2 + 1, b2 + 2, b2 + 3, b2 + 4]))

    arr = np.array(pts, dtype=dtype)
    vp = vtkPoints()
    vp.SetData(numpy_to_vtk(np.ascontiguousarray(arr), deep=1))
    ug = vtkUnstructuredGrid()
    ug.SetPoints(vp)
    ug.Allocate(len(cells))
    for ctype, ids in cells:
        idl = vtkIdList()
        for i in ids:
            idl.InsertNextId(i)
        ug.InsertNextCell(ctype, idl)
    scal = numpy_to_vtk(np.ascontiguousarray(arr[:, 2].copy()), deep=1)  # z as scalar
    scal.SetName("v")
    ug.GetPointData().SetScalars(scal)
    return ug


def make_structured_grid(n=24, dtype=np.float64):
    """A curvilinear vtkStructuredGrid on an n*n*n lattice carrying explicit
    per-point coordinates (so vtkExtractGrid's CopyPointsAndPointData copies real
    points, not implicit uniform-grid points) plus two deterministic point-data
    arrays. Coordinates use only integer/half-integer algebra (no trig) so the two
    backends start byte-identical. The requested dtype drives the point array
    precision, exercising both the float and double typed-pointer copy paths."""
    lin = np.arange(n, dtype=np.float64)
    gi, gj, gk = np.meshgrid(lin, lin, lin, indexing="ij")
    # Mild curvilinear warp via integer-fraction algebra (no transcendentals).
    x = gi + 0.25 * (gj % 3)
    y = gj + 0.5 * (gk % 2)
    z = gk + 0.125 * (gi % 4)
    # VTK structured-grid point order is x fastest, then y, then z.
    coords = np.stack(
        [
            x.transpose(2, 1, 0).ravel(),
            y.transpose(2, 1, 0).ravel(),
            z.transpose(2, 1, 0).ravel(),
        ],
        axis=1,
    ).astype(dtype)
    vp = vtkPoints()
    vp.SetData(numpy_to_vtk(np.ascontiguousarray(coords), deep=1))
    sg = vtkStructuredGrid()
    sg.SetDimensions(n, n, n)
    sg.SetPoints(vp)

    npts = n * n * n
    idx = np.arange(npts, dtype=np.int64)
    scal = numpy_to_vtk(np.ascontiguousarray((idx % 251).astype(dtype)), deep=1)
    scal.SetName("v")
    sg.GetPointData().SetScalars(scal)
    vec = np.empty((npts, 3), dtype=dtype)
    vec[:, 0] = (idx % 17).astype(dtype)
    vec[:, 1] = (idx % 13).astype(dtype)
    vec[:, 2] = (idx % 7).astype(dtype)
    va = numpy_to_vtk(np.ascontiguousarray(vec), deep=1)
    va.SetName("vec")
    sg.GetPointData().AddArray(va)
    return sg


def build_inputs_digest(dtype=np.float64):
    """Hash every constructed input array. Used by the harness to PROVE the two
    backends start from byte-identical inputs before attributing any output
    difference to the filter. Pure-numpy, backend-independent."""
    h = hashlib.sha256()
    for arr in (
        _radial_field(20, dtype),
        _ramp_field_2d(48, dtype),
        make_points_array(1500, dtype),
    ):
        h.update(np.ascontiguousarray(arr).tobytes())
    return h.hexdigest()


# ---------------------------------------------------------------------------
# Output capture: pull EVERY array (points, all point/cell data arrays, and
# topology) out of a vtkDataObject as raw numpy, for byte-exact comparison.
# ---------------------------------------------------------------------------
def _field_arrays(fd, tag):
    out = {}
    if fd is None:
        return out
    for i in range(fd.GetNumberOfArrays()):
        a = fd.GetArray(i)
        if a is None:
            # may be a non-numeric (string) array; skip — not part of compute path
            continue
        name = a.GetName() or f"arr{i}"
        out[f"{tag}:{name}"] = np.ascontiguousarray(vtk_to_numpy(a)).copy()
    return out


def capture_dataobject(obj):
    """Return {array_name: np.ndarray} for points + all data arrays + topology."""
    rec = {}
    if hasattr(obj, "GetPoints") and obj.GetPoints() is not None:
        rec["points"] = np.ascontiguousarray(
            vtk_to_numpy(obj.GetPoints().GetData())
        ).copy()
    if hasattr(obj, "GetPointData"):
        rec.update(_field_arrays(obj.GetPointData(), "pd"))
    if hasattr(obj, "GetCellData"):
        rec.update(_field_arrays(obj.GetCellData(), "cd"))

    # Topology / connectivity.
    if isinstance(obj, vtkPolyData):
        for tag, getter in (
            ("polys", obj.GetPolys),
            ("strips", obj.GetStrips),
            ("lines", obj.GetLines),
            ("verts", obj.GetVerts),
        ):
            ca = getter()
            if ca is not None and ca.GetNumberOfCells() > 0:
                rec[f"conn:{tag}"] = np.ascontiguousarray(
                    vtk_to_numpy(ca.GetConnectivityArray())
                ).copy()
                rec[f"off:{tag}"] = np.ascontiguousarray(
                    vtk_to_numpy(ca.GetOffsetsArray())
                ).copy()
    elif isinstance(obj, vtkUnstructuredGrid):
        ca = obj.GetCells()
        if ca is not None and ca.GetNumberOfCells() > 0:
            rec["conn"] = np.ascontiguousarray(
                vtk_to_numpy(ca.GetConnectivityArray())
            ).copy()
            rec["off"] = np.ascontiguousarray(
                vtk_to_numpy(ca.GetOffsetsArray())
            ).copy()
        ct = obj.GetCellTypesArray()
        if ct is not None:
            rec["celltypes"] = np.ascontiguousarray(vtk_to_numpy(ct)).copy()
    return rec


def _as_arrays(result):
    """Normalize an op return value to a {name: ndarray} dict."""
    if isinstance(result, dict):
        return {
            k: np.ascontiguousarray(np.asarray(v)).copy() for k, v in result.items()
        }
    return capture_dataobject(result)


# ===========================================================================
# OPERATION DEFINITIONS
# ===========================================================================
# Each op: fn(dtype, size) -> vtkDataObject | {name: ndarray}.
# `size` is a small integer resolution knob; meaning is per-op but monotonic.

# ---- The 9 MODIFIED filters (hard gate) ----
def op_decimate(dtype, size):
    d = vtkQuadricDecimation()
    d.SetInputData(make_sphere(size, size))
    d.SetTargetReduction(0.5)
    d.Update()
    return d.GetOutput()


def op_smooth(dtype, size):
    s = vtkSmoothPolyDataFilter()
    s.SetInputData(make_sphere(size, size))
    s.SetNumberOfIterations(40)
    s.Update()
    return s.GetOutput()


def op_normals(dtype, size):
    n = vtkPolyDataNormals()
    n.SetInputData(make_sphere(size, size))
    n.Update()
    return n.GetOutput()


def _sphere_with_precision(theta, phi, dtype):
    """Triangulated sphere whose POINT array is forced to the requested precision.

    vtkSphereSource emits float32 points; this rebuilds the points as a concrete
    AOS float32 OR float64 array so the vtkPolyDataNormals cell-normal fast path
    (FastDownCast<float>/FastDownCast<double>) is exercised on BOTH branches.
    """
    s = make_sphere(theta, phi)
    pts_np = vtk_to_numpy(s.GetPoints().GetData()).astype(dtype)
    pa = numpy_to_vtk(np.ascontiguousarray(pts_np), deep=1)
    newpts = vtkPoints()
    newpts.SetData(pa)
    out = vtkPolyData()
    out.SetPoints(newpts)
    out.SetPolys(s.GetPolys())
    return out


def op_normals_fastpath(dtype, size):
    """Covers the always-on bit-exact raw-pointer cell-normal fast path in
    vtkPolyDataNormals::GetCellNormals.

    Drives concrete AOS float32/float64 triangle meshes (the fast-path guard) and
    computes BOTH cell + point normals with sharp-edge splitting ON, so the cell
    normals produced by the hoisted-dispatch kernel feed the splitting + point
    accumulation. Output point + cell normal arrays must be byte-identical
    (maxULP=0) to stock vtkPolygon::ComputeNormal."""
    n = vtkPolyDataNormals()
    n.SetInputData(_sphere_with_precision(size, size, dtype))
    n.SetComputePointNormals(True)
    n.SetComputeCellNormals(True)
    n.SetSplitting(True)
    n.SetFeatureAngle(30.0)
    n.Update()
    return n.GetOutput()


def op_normals_smooth(dtype, size):
    """Companion to op_normals_fastpath with splitting OFF (smooth shading):
    same hoisted cell-normal kernel, no seam duplication. float32/float64."""
    n = vtkPolyDataNormals()
    n.SetInputData(_sphere_with_precision(size, size, dtype))
    n.SetComputePointNormals(True)
    n.SetComputeCellNormals(True)
    n.SetSplitting(False)
    n.Update()
    return n.GetOutput()


def op_contour(dtype, size):
    c = vtkContourFilter()
    c.SetInputData(make_volume(size, dtype))
    c.GenerateValues(8, 0.2 * size, 0.45 * size)
    c.Update()
    return c.GetOutput()


def op_clip(dtype, size):
    p = vtkPlane()
    p.SetOrigin(size / 2.0, size / 2.0, size / 2.0)
    p.SetNormal(1, 0, 0)
    c = vtkClipDataSet()
    c.SetInputData(make_volume(size, dtype))
    c.SetClipFunction(p)
    c.Update()
    return c.GetOutput()


def op_threshold(dtype, size):
    t = vtkThreshold()
    t.SetInputData(make_volume(size, dtype))
    t.SetLowerThreshold(0.15 * size)
    t.SetUpperThreshold(0.40 * size)
    t.Update()
    return t.GetOutput()


def op_threshold_ugrid(dtype, size):
    # Threshold a vtkUnstructuredGrid of hexahedra on its radial point scalar.
    # Unlike op_threshold (vtkImageData), a UG input drives the devirtualized
    # GetCellType/GetCellPoints fast path in vtkThreshold's EvaluateCellsFunctor.
    # The mid-band range keeps a partial shell of cells (non-trivial output).
    t = vtkThreshold()
    t.SetInputData(make_hex_ugrid(size, dtype))
    t.SetLowerThreshold(3.0)
    t.SetUpperThreshold(0.45 * size * size)
    t.Update()
    return t.GetOutput()


def _hex_ugrid_with_extra_data(size, dtype):
    """make_hex_ugrid plus extra point AND cell data so the threshold extraction
    must copy non-scalar arrays through vtkExtractCells' presized/devirtualized
    point + cell copy loops. All arrays are pure-integer algebra (deterministic,
    no trig) so the two backends start from byte-identical inputs."""
    ug = make_hex_ugrid(size, dtype)
    npts = ug.GetNumberOfPoints()
    ncells = ug.GetNumberOfCells()

    # extra 3-component point array (so the point-data copy moves a multi-comp tuple)
    pidx = np.arange(npts, dtype=dtype)
    pvec = np.ascontiguousarray(
        np.stack([pidx, 2.0 * pidx, pidx * pidx], axis=1).astype(dtype)
    )
    parr = numpy_to_vtk(pvec, deep=1)
    parr.SetName("pvec")
    ug.GetPointData().AddArray(parr)

    # cell scalar 'cv' (drives the cell-scalar criterion path) + a 2-comp extra
    # cell array that must be carried through to the output cell data.
    cidx = np.arange(ncells, dtype=dtype)
    cv = numpy_to_vtk(np.ascontiguousarray(cidx), deep=1)
    cv.SetName("cv")
    ug.GetCellData().SetScalars(cv)
    cextra = np.ascontiguousarray(
        np.stack([cidx, ncells - 1 - cidx], axis=1).astype(dtype)
    )
    carr = numpy_to_vtk(cextra, deep=1)
    carr.SetName("cextra")
    ug.GetCellData().AddArray(carr)
    return ug


def op_threshold_ugrid_pointdata(dtype, size):
    # Point-scalar threshold over a hex UG carrying extra point + cell data.
    # Default AllScalars=On: a cell passes only if ALL its point scalars satisfy
    # the criterion. Exercises vtkExtractCells' point-map / cell copy loops with
    # non-trivial point AND cell data to carry through.
    ug = _hex_ugrid_with_extra_data(size, dtype)
    t = vtkThreshold()
    t.SetInputData(ug)
    t.SetInputArrayToProcess(
        0, 0, 0, vtkDataObject.FIELD_ASSOCIATION_POINTS, "v"
    )
    t.SetAllScalars(1)
    t.SetLowerThreshold(3.0)
    t.SetUpperThreshold(0.45 * size * size)
    t.Update()
    return t.GetOutput()


def op_threshold_ugrid_pointdata_any(dtype, size):
    # Same as above but AllScalars=Off: a cell passes if ANY point scalar passes.
    # Different selection set -> distinct cell/point order through the copy loops.
    ug = _hex_ugrid_with_extra_data(size, dtype)
    t = vtkThreshold()
    t.SetInputData(ug)
    t.SetInputArrayToProcess(
        0, 0, 0, vtkDataObject.FIELD_ASSOCIATION_POINTS, "v"
    )
    t.SetAllScalars(0)
    t.SetLowerThreshold(3.0)
    t.SetUpperThreshold(0.45 * size * size)
    t.Update()
    return t.GetOutput()


def op_threshold_ugrid_celldata(dtype, size):
    # Cell-scalar threshold over the same UG: drives the "use cell scalars"
    # branch (EvaluateComponents(scalars, cellId)) and a different kept-cell set,
    # so the devirtualized vtkExtractCells copy loops carry the extra point + cell
    # data for a cell-criterion selection.
    ug = _hex_ugrid_with_extra_data(size, dtype)
    t = vtkThreshold()
    t.SetInputData(ug)
    t.SetInputArrayToProcess(
        0, 0, 0, vtkDataObject.FIELD_ASSOCIATION_CELLS, "cv"
    )
    ncells = (size - 1) ** 3
    t.SetLowerThreshold(0.2 * ncells)
    t.SetUpperThreshold(0.8 * ncells)
    t.Update()
    return t.GetOutput()


def op_warp(dtype, size):
    w = vtkWarpScalar()
    w.SetInputData(make_grid2d(size, dtype))
    w.SetScaleFactor(0.5)
    w.Update()
    return w.GetOutput()


def op_glyph(dtype, size):
    a = vtkArrowSource()
    g = vtkGlyph3D()
    g.SetInputData(make_sphere(size, size))
    g.SetSourceConnection(a.GetOutputPort())
    g.SetScaleFactor(0.1)
    g.Update()
    return g.GetOutput()


def _tcoord_quad_source():
    """A tiny deterministic glyph source polydata that carries TCoords, so a
    vtkGlyph3D over it populates the output TCoords array (the plain arrow/sphere
    sources do not)."""
    src = vtkPolyData()
    pts = vtkPoints()
    pts.SetDataTypeToDouble()
    pts.InsertNextPoint(0.0, 0.0, 0.0)
    pts.InsertNextPoint(1.0, 0.0, 0.0)
    pts.InsertNextPoint(1.0, 1.0, 0.0)
    pts.InsertNextPoint(0.0, 1.0, 0.0)
    src.SetPoints(pts)
    cells = vtkCellArray()
    cells.InsertNextCell(4)
    for k in range(4):
        cells.InsertCellPoint(k)
    src.SetPolys(cells)
    tc = numpy_to_vtk(
        np.array([[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]], dtype=np.float64),
        deep=1,
    )
    tc.SetName("TCoords")
    src.GetPointData().SetTCoords(tc)
    return src


def op_glyph_arrays(dtype, size):
    """Glyph whose output exercises the pre-sized per-tuple arrays the wave-8
    vtkGlyph3D optimization touches: GlyphVector (newVectors), the COLOR_BY_VECTOR
    magnitude scalars (newScalars), and TCoords (newTCoords). The plain ``glyph``
    op produces none of these, so this case is what makes the wave-8 SetTuple /
    pre-size / trim path non-vacuously covered."""
    inp = make_sphere_with_vectors(size, size, dtype)
    g = vtkGlyph3D()
    g.SetInputData(inp)
    g.SetSourceData(_tcoord_quad_source())
    g.SetVectorModeToUseVector()
    g.SetColorModeToColorByVector()
    g.SetScaleModeToScaleByVector()
    g.SetScaleFactor(0.2)
    g.OrientOn()
    g.Update()
    return g.GetOutput()


def _mixed_cell_glyph_source():
    """A tiny glyph source polydata carrying VTK_VERTEX, VTK_LINE and a triangle
    cell, so a vtkGlyph3D over it routes output cells into THREE different
    vtkPolyData target cell arrays (Verts, Lines, Polys). This exercises the
    multi-target routing + per-target typed append of vtkPolyData::
    InsertNextCellBlock (the batched glyph cell-append path); the plain arrow/
    quad sources are single-target (Polys/Lines) only."""
    src = vtkPolyData()
    pts = vtkPoints()
    pts.SetDataTypeToDouble()
    pts.InsertNextPoint(0.0, 0.0, 0.0)
    pts.InsertNextPoint(1.0, 0.0, 0.0)
    pts.InsertNextPoint(1.0, 1.0, 0.0)
    pts.InsertNextPoint(0.0, 1.0, 0.0)
    src.SetPoints(pts)

    verts = vtkCellArray()
    verts.InsertNextCell(1)
    verts.InsertCellPoint(0)
    src.SetVerts(verts)

    lines = vtkCellArray()
    lines.InsertNextCell(2)
    lines.InsertCellPoint(1)
    lines.InsertCellPoint(2)
    src.SetLines(lines)

    polys = vtkCellArray()
    polys.InsertNextCell(3)
    for k in (0, 2, 3):
        polys.InsertCellPoint(k)
    src.SetPolys(polys)
    return src


def op_glyph_mixedcells(dtype, size):
    """vtkGlyph3D over a source whose cells span Verts+Lines+Polys, so the output
    cell append routes into all three target cell arrays. Makes vtkPolyData::
    InsertNextCellBlock's multi-target typed append non-vacuously covered (the
    plain ``glyph`` op uses an all-triangle arrow -> Polys only)."""
    g = vtkGlyph3D()
    g.SetInputData(make_sphere(size, size))
    g.SetSourceData(_mixed_cell_glyph_source())
    g.SetScaleFactor(0.1)
    g.Update()
    return g.GetOutput()


def op_cell2point(dtype, size):
    vol = make_volume(size, dtype)
    nc = vol.GetNumberOfCells()
    ca = numpy_to_vtk(np.linspace(0, 1, nc).astype(dtype), deep=1)
    ca.SetName("c")
    vol.GetCellData().SetScalars(ca)
    f = vtkCellDataToPointData()
    f.SetInputData(vol)
    f.Update()
    return f.GetOutput()


# ---- Broader filter coverage (non-gate, but still must be bit-exact) ----
def op_point2cell(dtype, size):
    f = vtkPointDataToCellData()
    f.SetInputData(make_volume(size, dtype))
    f.Update()
    return f.GetOutput()


def op_point2cell_ugrid(dtype, size):
    # vtkPointDataToCellData over a vtkUnstructuredGrid of hexahedra. Unlike
    # op_point2cell (vtkImageData), a UG input drives the devirtualized
    # GetCellPoints fast path in PointDataToCellDataFunctor (cached
    # vtkCellArray::GetCellAtId rather than the virtual vtkDataSet::GetCellPoints).
    # A multi-component point-data array (in addition to the radial scalar 'v')
    # makes the per-cell ArrayList.Average non-trivial across more than one array.
    ug = make_hex_ugrid(size, dtype)
    pts = vtk_to_numpy(ug.GetPoints().GetData())
    vec = np.ascontiguousarray(
        np.stack(
            [pts[:, 0] * 2.0, pts[:, 1] - pts[:, 2], pts[:, 0] + pts[:, 1] + pts[:, 2]],
            axis=1,
        )
    ).astype(dtype)
    va = numpy_to_vtk(vec, deep=1)
    va.SetName("vec3")
    ug.GetPointData().AddArray(va)
    f = vtkPointDataToCellData()
    f.SetInputData(ug)
    f.Update()
    return f.GetOutput()


def op_elevation(dtype, size):
    # Force the input POINT array to the requested precision so BOTH the float32
    # and float64 typed point-array branches of the vtkArrayDispatch fast path in
    # vtkElevationFilter are exercised (vtkSphereSource alone emits float32 only).
    # Non-trivial, non-axis-aligned LowPoint/HighPoint so the projection/clamp/
    # normalize arithmetic is exercised with all three diffVector components and a
    # genuine [0,1] clamp on both ends of the sphere.
    f = vtkElevationFilter()
    f.SetInputData(_sphere_with_precision(size, size, dtype))
    f.SetLowPoint(-0.3, -0.2, -0.5)
    f.SetHighPoint(0.4, 0.25, 0.5)
    f.SetScalarRange(0.0, 7.0)
    f.Update()
    return f.GetOutput()


def op_simple_elevation(dtype, size):
    # vtkSimpleElevationFilter: per-point scalar = dot(point, Vector). Force the
    # input point precision to `dtype` so both the float32 and float64 typed
    # point-array branches of the vtkArrayDispatch fast path are exercised. A
    # non-axis-aligned Vector with all three components nonzero makes the dot
    # product non-trivial (v[0]*p[0] + v[1]*p[1] + v[2]*p[2] in that exact order).
    f = vtkSimpleElevationFilter()
    f.SetInputData(_sphere_with_precision(size, size, dtype))
    f.SetVector(0.3, -0.5, 0.81)
    f.Update()
    return f.GetOutput()


def op_warpvector(dtype, size):
    w = vtkWarpVector()
    w.SetInputData(make_sphere_with_vectors(size, size, dtype))
    w.SetScaleFactor(0.3)
    w.Update()
    return w.GetOutput()


def make_sphere_scalar_normals(theta=40, phi=40, dtype=np.float64):
    """Sphere carrying both point normals AND a per-point scalar 's', so
    vtkWarpScalar exercises the per-point data-normals path (inNormals !=
    nullptr): xo = xi + sf*s*normal(ptId). Coordinates are the source's; the
    scalar uses only the z coordinate (no trig) so both backends start
    byte-identical."""
    s = make_sphere(theta, phi)
    n = vtkPolyDataNormals()
    n.SetInputData(s)
    n.SetComputePointNormals(True)
    n.Update()
    out = n.GetOutput()
    coords = vtk_to_numpy(out.GetPoints().GetData())
    scal = numpy_to_vtk(np.ascontiguousarray(coords[:, 2]).astype(dtype), deep=1)
    scal.SetName("s")
    out.GetPointData().SetScalars(scal)
    return out


def op_warpscalar_normals(dtype, size):
    """vtkWarpScalar over a point set that carries point normals, so the
    per-point inNormals->GetTuple path (not the constant-normal fast path) is
    exercised. UseNormal stays off so the data normals are used."""
    w = vtkWarpScalar()
    w.SetInputData(make_sphere_scalar_normals(size, size, dtype))
    w.SetScaleFactor(0.7)
    w.Update()
    return w.GetOutput()


def op_transform(dtype, size):
    """vtkTransformFilter with a non-trivial 4x4 (rotate+translate+scale) so the
    full matrix*point math (vtkLinearTransform::TransformPoints, the fvtk AVX2
    FMV kernel) is exercised. Covers the FMV'd compute-bound transform kernel."""
    t = vtkTransform()
    t.Translate(0.123, -0.456, 0.789)
    t.RotateWXYZ(37.0, 0.3, 0.5, 0.81)
    t.Scale(1.25, 0.875, 1.0625)
    f = vtkTransformFilter()
    f.SetTransform(t)
    f.SetInputData(make_sphere(size, size))
    if dtype == "float64":
        f.SetOutputPointsPrecision(1)  # DOUBLE
    else:
        f.SetOutputPointsPrecision(2)  # SINGLE
    f.Update()
    return f.GetOutput()


def _make_linear_transform():
    t = vtkTransform()
    t.Translate(0.123, -0.456, 0.789)
    t.RotateWXYZ(37.0, 0.3, 0.5, 0.81)
    t.Scale(1.25, 0.875, 1.0625)
    return t


def op_transform_nv(dtype, size):
    """vtkTransformFilter (linear) over a sphere carrying BOTH point normals and a
    point vectors array, so TransformPointsNormalsVectors (points + normals +
    vectors) is exercised. The linear path is already devirtualized; this guards
    its bit-exactness alongside the nonlinear ops below."""
    f = vtkTransformFilter()
    f.SetTransform(_make_linear_transform())
    f.SetInputData(make_sphere_with_vectors(size, size, dtype))
    f.SetOutputPointsPrecision(1 if dtype == "float64" else 2)
    f.Update()
    return f.GetOutput()


def op_transform_pdf(dtype, size):
    """vtkTransformPolyDataFilter (linear) with point normals + vectors. Covers the
    second target filter; its normals/vectors output arrays are always float."""
    f = vtkTransformPolyDataFilter()
    f.SetTransform(_make_linear_transform())
    f.SetInputData(make_sphere_with_vectors(size, size, dtype))
    f.SetOutputPointsPrecision(1 if dtype == "float64" else 2)
    f.Update()
    return f.GetOutput()


def op_transform_perspective(dtype, size):
    """vtkTransformFilter with a vtkPerspectiveTransform (a vtkHomogeneousTransform,
    non-linear / perspective divide). Exercises the devirtualized AOS gather/scatter
    in vtkHomogeneousTransform::TransformPointsNormalsVectors (points + normals +
    vectors all go through the homogeneous loop)."""
    t = vtkPerspectiveTransform()
    t.Translate(0.123, -0.456, 0.789)
    t.RotateWXYZ(37.0, 0.3, 0.5, 0.81)
    t.Scale(1.25, 0.875, 1.0625)
    # introduce a non-affine row so the homogeneous w-divide is non-trivial
    m = t.GetMatrix()
    m.SetElement(3, 0, 0.05)
    m.SetElement(3, 1, -0.03)
    m.SetElement(3, 2, 0.02)
    t.SetMatrix(m)
    f = vtkTransformFilter()
    f.SetTransform(t)
    f.SetInputData(make_sphere_with_vectors(size, size, dtype))
    f.SetOutputPointsPrecision(1 if dtype == "float64" else 2)
    f.Update()
    return f.GetOutput()


def op_transform_tps(dtype, size):
    """vtkTransformFilter with a vtkThinPlateSplineTransform (a truly nonlinear
    vtkAbstractTransform subclass that does NOT override TransformPoints/
    TransformPointsNormalsVectors). Exercises the devirtualized AOS gather/scatter
    in vtkAbstractTransform's generic per-point loop (InternalTransformPoint /
    InternalTransformDerivative stay virtual and bit-identical)."""
    src = vtkPoints()
    dst = vtkPoints()
    # a small deterministic control-point cage around the unit sphere
    cage = [
        (-1.0, -1.0, -1.0),
        (1.0, -1.0, -1.0),
        (-1.0, 1.0, -1.0),
        (1.0, 1.0, -1.0),
        (-1.0, -1.0, 1.0),
        (1.0, -1.0, 1.0),
        (-1.0, 1.0, 1.0),
        (1.0, 1.0, 1.0),
    ]
    for i, (x, y, z) in enumerate(cage):
        src.InsertNextPoint(x, y, z)
        # deterministic warp of each control point
        dst.InsertNextPoint(
            x + 0.10 * ((i % 3) - 1),
            y - 0.07 * ((i % 2)),
            z + 0.05 * (((i + 1) % 3) - 1),
        )
    tps = vtkThinPlateSplineTransform()
    tps.SetSourceLandmarks(src)
    tps.SetTargetLandmarks(dst)
    tps.SetBasisToR()  # R basis is well-defined in 3D
    f = vtkTransformFilter()
    f.SetTransform(tps)
    f.SetInputData(make_sphere_with_vectors(size, size, dtype))
    f.SetOutputPointsPrecision(1 if dtype == "float64" else 2)
    f.Update()
    return f.GetOutput()


def op_clean(dtype, size):
    c = vtkCleanPolyData()
    c.SetInputData(make_sphere(size, size))
    c.Update()
    return c.GetOutput()


def op_triangle(dtype, size):
    s = vtkSphereSource()
    s.SetThetaResolution(size)
    s.SetPhiResolution(size)
    s.Update()
    t = vtkTriangleFilter()
    t.SetInputConnection(s.GetOutputPort())
    t.Update()
    return t.GetOutput()


def op_geometry(dtype, size):
    g = vtkGeometryFilter()
    g.SetInputData(make_volume(size, dtype))
    g.Update()
    return g.GetOutput()


def op_shrink(dtype, size):
    f = vtkShrinkFilter()
    f.SetInputData(make_volume(size, dtype))
    f.SetShrinkFactor(0.8)
    f.Update()
    return f.GetOutput()


def op_connectivity(dtype, size):
    c = vtkConnectivityFilter()
    c.SetInputData(make_volume(size, dtype))
    c.SetExtractionModeToAllRegions()
    c.Update()
    return c.GetOutput()


def op_connectivity_largest(dtype, size):
    # vtkConnectivityFilter in the default ExtractLargestRegion mode over a hex
    # UG -> the "extract largest region" output branch, whose per-cell
    # GetCellType is hoisted (read once, reused for the polyhedron test and the
    # InsertNextCell emit). Single connected grid => the whole grid is the
    # largest region, so every cell flows through the hoisted branch.
    c = vtkConnectivityFilter()
    c.SetInputData(make_hex_ugrid(size, dtype))
    c.SetExtractionModeToLargestRegion()
    c.Update()
    return c.GetOutput()


def op_featureedges(dtype, size):
    f = vtkFeatureEdges()
    f.SetInputData(make_sphere(size, size))
    f.SetFeatureAngle(20.0)
    f.Update()
    return f.GetOutput()


def op_stripper(dtype, size):
    f = vtkStripper()
    f.SetInputData(make_sphere(size, size))
    f.Update()
    return f.GetOutput()


def op_vertexglyph(dtype, size):
    f = vtkVertexGlyphFilter()
    f.SetInputData(make_sphere(size, size))
    f.Update()
    return f.GetOutput()


def op_decimatepro(dtype, size):
    d = vtkDecimatePro()
    d.SetInputData(make_sphere(size, size))
    d.SetTargetReduction(0.6)
    d.Update()
    return d.GetOutput()


def op_cone_triangulate(dtype, size):
    c = vtkConeSource()
    c.SetResolution(size)
    c.Update()
    t = vtkTriangleFilter()
    t.SetInputConnection(c.GetOutputPort())
    t.Update()
    return t.GetOutput()


def make_nested_loops(nrings, perside, dtype):
    """A vtkPolyData of `nrings` concentric, closed, axis-aligned square line
    loops centered on the origin, each loop a single closed line cell with
    `perside` segments per side. Coordinates are pure integer algebra so both
    backends start byte-identical. The loops nest inside one another, so
    vtkContourTriangulator must run its O(n^2) poly-in-poly grouping to decide
    which loops are holes -- the path whose per-vertex vtkPoints::GetPoint this
    change devirtualizes."""
    pts = []
    cells = []  # list of point-id lists, one closed loop each
    for r in range(nrings):
        half = (nrings - r) * 4  # outer rings are larger
        base = len(pts)
        loop = []
        # walk the square perimeter as integer lattice points
        per = []
        for i in range(perside):
            t = i * (2 * half) // perside
            per.append((-half + t, -half))  # bottom edge L->R
        for i in range(perside):
            t = i * (2 * half) // perside
            per.append((half, -half + t))  # right edge B->T
        for i in range(perside):
            t = i * (2 * half) // perside
            per.append((half - t, half))  # top edge R->L
        for i in range(perside):
            t = i * (2 * half) // perside
            per.append((-half, half - t))  # left edge T->B
        for (x, y) in per:
            pts.append((x, y, 0))
            loop.append(base + len(loop))
        loop.append(base)  # close the loop back to its first vertex
        cells.append(loop)
    coords = np.array(pts, dtype=dtype)
    pd = vtkPolyData()
    vp = vtkPoints()
    vp.SetData(numpy_to_vtk(np.ascontiguousarray(coords), deep=1))
    pd.SetPoints(vp)
    lines = vtkCellArray()
    for loop in cells:
        ids = vtkIdList()
        for pid in loop:
            ids.InsertNextId(pid)
        lines.InsertNextCell(ids)
    pd.SetLines(lines)
    return pd


def op_contour_triangulator(dtype, size):
    # vtkContourTriangulator on nested closed line loops. The concentric loops
    # force the vtkCCSMakeHoleyPolys / vtkCCSPolyInPoly grouping (O(n^2) over the
    # polygon vertices), which fetches every vertex coordinate via the
    # devirtualized vtkCCSPointReader (was vtkPoints::GetPoint -> virtual
    # GetTuple). `size` -> segments-per-side; dtype drives the point-array
    # precision so both the float and double reader branches are exercised.
    ct = vtkContourTriangulator()
    ct.SetInputData(make_nested_loops(nrings=4, perside=size, dtype=dtype))
    ct.Update()
    return ct.GetOutput()


def op_tube(dtype, size):
    # `size` -> polyline length; a few lines so the per-vertex point-data copy
    # path (the optimized InsertTuple loop) runs over many generated vertices.
    t = vtkTubeFilter()
    t.SetInputData(make_polylines(nlines=6, length=size, dtype=dtype))
    t.SetNumberOfSides(8)
    t.SetRadius(0.2)
    t.SetVaryRadiusToVaryRadiusByScalar()
    t.SetCapping(1)
    t.Update()
    return t.GetOutput()


def op_gradient(dtype, size):
    g = vtkGradientFilter()
    g.SetInputData(make_volume(size, dtype))
    g.Update()
    return g.GetOutput()


def op_clip_tets(dtype, size):
    # vtkClipDataSet on a tetrahedral ugrid -> per-cell vtkTetra::Clip (the
    # cached-endpoint-scalar optimization in Common/DataModel/vtkTetra.cxx).
    p = vtkPlane()
    c = (size - 1) / 2.0
    p.SetOrigin(c, c, c)
    p.SetNormal(1, 1, 1)
    cl = vtkClipDataSet()
    cl.SetInputData(make_tet_ugrid(size, dtype))
    cl.SetClipFunction(p)
    cl.Update()
    return cl.GetOutput()


def op_contour_hexug(dtype, size):
    # vtkContourGrid on a hex ugrid -> per-cell vtkHexahedron::Contour (the
    # cached-endpoint-scalar optimization). vtkContourGrid reaches the cell
    # method (vtkContourFilter would bypass it via the linear-grid fast path).
    cg = vtkContourGrid()
    cg.SetInputData(make_hex_ugrid(size, dtype))
    cg.SetValue(0, 0.25 * (size ** 2))
    cg.Update()
    return cg.GetOutput()


def op_contour_tetug(dtype, size):
    # vtkContourGrid on a tet ugrid -> per-cell vtkTetra::Contour.
    cg = vtkContourGrid()
    cg.SetInputData(make_tet_ugrid(size, dtype))
    cg.SetValue(0, 0.25 * (size ** 2))
    cg.Update()
    return cg.GetOutput()


def op_append(dtype, size):
    # vtkAppendFilter merging POLYDATA inputs -> the vtkPolyData branch's per-cell
    # cell-type copy loop (the cached base-pointer optimization in
    # Filters/Core/vtkAppendFilter.cxx lives in that branch, not the UG branch).
    a = vtkAppendFilter()
    a.AddInputData(make_sphere(size, size))
    a.AddInputData(make_sphere(size + 4, size + 4))
    a.MergePointsOn()
    a.Update()
    return a.GetOutput()


def _ugrid_with_data(maker, n, dtype, pd_name, cd_base, dtype_cd=np.float64):
    """Take a UG builder, attach a deterministic per-POINT scalar named `pd_name`
    and a deterministic per-CELL scalar 'cs' (values cd_base + cellId) so the
    append's point-data / cell-data CopyData paths copy non-trivial, per-input
    distinguishable values. Order/offset bugs would scramble these."""
    ug = maker(n, dtype)
    npts = ug.GetNumberOfPoints()
    ncells = ug.GetNumberOfCells()
    pa = numpy_to_vtk(
        np.ascontiguousarray((np.arange(npts, dtype=np.int64) % 17).astype(dtype)),
        deep=1,
    )
    pa.SetName(pd_name)
    ug.GetPointData().AddArray(pa)
    ca = numpy_to_vtk(
        np.ascontiguousarray(
            (cd_base + np.arange(ncells, dtype=np.int64)).astype(dtype_cd)
        ),
        deep=1,
    )
    ca.SetName("cs")
    ug.GetCellData().AddArray(ca)
    return ug


def op_append_ugrid_mixed(dtype, size):
    """vtkAppendFilter over UNSTRUCTURED-GRID inputs of DIFFERENT cell types
    (hexahedra + wedge/pyramid), NO point merge. Drives the vtkUnstructuredGrid
    fast branch of RequestData (typed vtkCellArray bulk append via AppendCellArray
    + per-input cellConnectivityOffset/pointOffset) for two distinct cell-type
    topologies concatenated in order, plus the AppendArrays/FieldList.CopyData
    point+cell-data copy into presized output arrays.

    Bit-exact load: the output points, connectivity, offsets, celltypes and the
    'cs' cell scalar (= input-index-dependent base + cellId) must appear in input
    order with the correct per-input point-id offset. A wrong offset or a scrambled
    input order changes connectivity / cell-data bytes -> divergence. Both inputs
    carry a SHARED point scalar 'p' and cell scalar 'cs' (intersected field list),
    so every array is copied; the hex input additionally carries a 'hexonly' array
    that the wedge input lacks, exercising the field-intersection drop (it must NOT
    appear in the output)."""
    a = vtkAppendFilter()
    hexug = _ugrid_with_data(make_hex_ugrid, max(4, size), dtype, "p", 100)
    # extra array present on ONLY the first input -> must be intersected away.
    nptsh = hexug.GetNumberOfPoints()
    extra = numpy_to_vtk(
        np.ascontiguousarray((np.arange(nptsh, dtype=np.int64) % 5).astype(dtype)),
        deep=1,
    )
    extra.SetName("hexonly")
    hexug.GetPointData().AddArray(extra)
    wp = _ugrid_with_data(make_wedge_pyramid_ugrid, max(2, size // 4), dtype, "p", 500)
    a.AddInputData(hexug)
    a.AddInputData(wp)
    a.MergePointsOff()
    a.Update()
    return a.GetOutput()


def op_append_mixed_types(dtype, size):
    """vtkAppendFilter mixing a vtkUnstructuredGrid input AND a vtkPolyData input,
    NO merge. The UG drives the fast UG branch; the polydata drives the vtkPolyData
    branch (verts/lines/polys typed appends with per-array cell/connectivity offset
    bookkeeping). Both carry a shared point scalar 'p' and cell scalar 'cs'.

    This is the cross-branch order/offset test: output cells are [all UG cells,
    then all polydata cells]; the polydata's point ids must be offset by the UG
    point count. Any mismatch in branch ordering, cumulative offsets, or the
    cell-data concatenation order shows up as a connectivity / 'cs' byte diff."""
    a = vtkAppendFilter()
    hexug = _ugrid_with_data(make_hex_ugrid, max(4, size), dtype, "p", 0)
    pd = make_polylines(nlines=4, length=max(4, size // 2), dtype=dtype)
    # rename the polydata point scalar to the shared name 'p' so it intersects with
    # the UG's 'p'; add a matching per-cell 'cs' scalar.
    pd.GetPointData().GetScalars().SetName("p")
    nc = pd.GetNumberOfCells()
    ca = numpy_to_vtk(
        np.ascontiguousarray((9000 + np.arange(nc, dtype=np.int64)).astype(np.float64)),
        deep=1,
    )
    ca.SetName("cs")
    pd.GetCellData().AddArray(ca)
    a.AddInputData(hexug)
    a.AddInputData(pd)
    a.MergePointsOff()
    a.Update()
    return a.GetOutput()


def _polydata_vlp_with_data(seed, npts, dtype, scalar_name="p"):
    """A vtkPolyData carrying ALL of verts, lines and polys, plus a per-POINT
    scalar (name `scalar_name`) and a per-CELL scalar 'cs'. `seed` shifts the
    coordinates and the scalar values so distinct inputs are distinguishable in
    the appended output. Deterministic integer algebra only (no trig)."""
    idx = np.arange(npts, dtype=np.int64)
    coords = np.empty((npts, 3), dtype=dtype)
    coords[:, 0] = ((idx + seed) % 7).astype(dtype)
    coords[:, 1] = ((idx * 2 + seed) % 5).astype(dtype)
    coords[:, 2] = (idx % 3).astype(dtype)
    pd = vtkPolyData()
    vp = vtkPoints()
    vp.SetData(numpy_to_vtk(np.ascontiguousarray(coords), deep=1))
    pd.SetPoints(vp)

    verts = vtkCellArray()
    verts.InsertNextCell(1)
    verts.InsertCellPoint(0)
    verts.InsertNextCell(1)
    verts.InsertCellPoint(npts - 1)
    pd.SetVerts(verts)

    lines = vtkCellArray()
    lines.InsertNextCell(3)
    for k in (0, 1, 2):
        lines.InsertCellPoint(k % npts)
    pd.SetLines(lines)

    polys = vtkCellArray()
    polys.InsertNextCell(3)
    for k in (1, 2, 3):
        polys.InsertCellPoint(k % npts)
    polys.InsertNextCell(4)
    for k in (0, 2, 3, 4):
        polys.InsertCellPoint(k % npts)
    pd.SetPolys(polys)

    pa = numpy_to_vtk(
        np.ascontiguousarray(((idx + 100 * seed) % 23).astype(dtype)), deep=1
    )
    pa.SetName(scalar_name)
    pd.GetPointData().AddArray(pa)

    ncells = pd.GetNumberOfCells()
    ca = numpy_to_vtk(
        np.ascontiguousarray(
            (1000 * seed + np.arange(ncells, dtype=np.int64)).astype(np.float64)
        ),
        deep=1,
    )
    ca.SetName("cs")
    pd.GetCellData().AddArray(ca)
    return pd


def op_append_polydata(dtype, size):
    """vtkAppendPolyData over multiple vtkPolyData inputs, each carrying verts +
    lines + polys and a point scalar 'p' + cell scalar 'cs'. Exercises
    vtkAppendPolyData::ExecuteAppend (Filters/Core/vtkAppendPolyData.cxx): the
    presized point copy (InsertTuples into SetNumberOfPoints storage), the four
    per-target typed cell-array appends (AppendCellArray with per-input
    point/connectivity offsets), and the cell-data CopyData whose output offsets
    are the GROUPED order verts|lines|polys|strips across all inputs.

    Bit-exact load: the canonical vtkPolyData cell order is ALL verts, then ALL
    lines, then ALL polys, then ALL strips, concatenated across inputs within each
    group. The 'cs' cell scalar is copied with exactly that grouped, per-input
    offset layout (outVertOffset / outLineOffset+totalVerts / ...). A scrambled
    group order, a wrong per-input offset, or a wrong point-id offset on the
    connectivity all produce byte differences in conn/off/'cs'. Three inputs of
    different sizes make the cumulative offsets non-degenerate."""
    a = vtkAppendPolyData()
    a.AddInputData(_polydata_vlp_with_data(1, max(6, size), dtype))
    a.AddInputData(_polydata_vlp_with_data(2, max(6, size + 3), dtype))
    a.AddInputData(_polydata_vlp_with_data(3, max(6, size + 5), dtype))
    a.Update()
    return a.GetOutput()


def op_append_polydata_partial(dtype, size):
    """vtkAppendPolyData where inputs have DIFFERENT array sets and DIFFERENT
    present cell-array targets. Input 0 has verts+lines+polys and arrays {p, cs};
    input 1 is polys-only and arrays {p} (no 'cs'). The point-field intersection
    keeps only 'p'; 'cs' is dropped (present in some inputs but not all). This
    exercises vtkAppendPolyData's FieldList intersection / NULL-array handling and
    the totalNumberOfVerts/Lines==per-input-only path (an input contributing to
    only a subset of the four cell-array targets).

    Bit-exact load: output retains exactly the intersected point array 'p' in
    input order; cell array 'cs' must be absent; cells are grouped verts|lines|
    polys across both inputs with correct offsets even though input 1 has no
    verts/lines."""
    a = vtkAppendPolyData()
    pd0 = _polydata_vlp_with_data(4, max(6, size), dtype)
    pd1 = _polydata_vlp_with_data(5, max(6, size + 2), dtype)
    # input 1: drop its cell scalar 'cs' (so it is intersected away) and keep
    # ONLY polys (no verts/lines) to exercise the subset-of-targets path.
    pd1.GetCellData().RemoveArray("cs")
    pd1.SetVerts(vtkCellArray())
    pd1.SetLines(vtkCellArray())
    a.AddInputData(pd0)
    a.AddInputData(pd1)
    a.Update()
    return a.GetOutput()


def op_probe(dtype, size):
    # vtkProbeFilter probing an image-data input against an unstructured tet
    # source -> ProbeImagePointsInCell (the devirtualized ComputePointId path in
    # Filters/Core/vtkProbeFilter.cxx). Image + tet grid share the [0,size-1]^3
    # extent so points fall inside source cells.
    pr = vtkProbeFilter()
    pr.SetInputData(make_volume(size, dtype))
    pr.SetSourceData(make_tet_ugrid(size, dtype))
    pr.Update()
    return pr.GetOutput()


def op_geometry_ugrid(dtype, size):
    # vtkGeometryFilter over an unstructured hex grid densely iterates every cell
    # via vtkUnstructuredGrid::GetCellPoints -> vtkCellArray::GetCellAtId (the
    # inline operator[] connectivity-read optimization in vtkCellArray.h).
    g = vtkGeometryFilter()
    g.SetInputData(make_hex_ugrid(size, dtype))
    g.Update()
    return g.GetOutput()


def op_tableclip_ugrid(dtype, size):
    # vtkTableBasedClipDataSet (pyvista's default clip) on a hex UG -> the
    # ClipTDataSet<vtkUnstructuredGrid> instantiation, whose EvaluateCells /
    # ExtractCells per-cell GetCellType/GetCellPoints are devirtualized via
    # if constexpr. The edge-interpolation FP is untouched.
    p = vtkPlane()
    p.SetOrigin(size / 2.0, size / 2.0, size / 2.0)
    p.SetNormal(1, 1, 0)
    cl = vtkTableBasedClipDataSet()
    cl.SetInputData(make_hex_ugrid(size, dtype))
    cl.SetClipFunction(p)
    cl.Update()
    return cl.GetOutput()


def op_datasetsurface_ugrid(dtype, size):
    # vtkDataSetSurfaceFilter (NOT vtkGeometryFilter — it has its own UG path)
    # directly over a hex UG drives UnstructuredGridExecuteInternal, whose dense
    # per-cell GetCellType/GetCellPoints are devirtualized for the concrete UG.
    s = vtkDataSetSurfaceFilter()
    s.SetInputData(make_hex_ugrid(size, dtype))
    s.Update()
    return s.GetOutput()


def op_contour_wedgepyr(dtype, size):
    # vtkContourGrid on an explicit WEDGE+PYRAMID ugrid -> per-cell
    # vtkWedge::Contour / vtkPyramid::Contour (cached-endpoint-scalar opt).
    cg = vtkContourGrid()
    cg.SetInputData(make_wedge_pyramid_ugrid(size, dtype))
    # z-scalar in [0,1] for the first wedge and [0,1.5] for the first pyramid;
    # 0.5 cuts straight through both cell types' edges.
    cg.SetValue(0, 0.5)
    cg.Update()
    return cg.GetOutput()


def op_clip_multicomp(dtype, size):
    # vtkClipDataSet on a volume carrying a 3-component point array -> the new
    # edge points interpolate that array via vtkGenericDataArray::InterpolateTuple
    # with numComps=3 (the loop-interchange optimization's multi-component path).
    vol = make_volume(size, dtype)
    npts = vol.GetNumberOfPoints()
    idx = np.arange(npts, dtype=np.int64)
    vec = np.stack([idx % 7, (idx * 2) % 11, (idx * 3) % 13], axis=1).astype(dtype)
    va = numpy_to_vtk(np.ascontiguousarray(vec), deep=1)
    va.SetName("vec")
    vol.GetPointData().AddArray(va)
    p = vtkPlane()
    c = (size - 1) / 2.0
    p.SetOrigin(c, c, c)
    p.SetNormal(1, 1, 0)
    cl = vtkClipDataSet()
    cl.SetInputData(vol)
    cl.SetClipFunction(p)
    cl.Update()
    return cl.GetOutput()


def op_polydata_celltypes(dtype, size):
    # Build a vtkPolyData carrying ALL FOUR cell-array targets (verts, lines,
    # polys, strips) and read every cell back via GetCellPoints. This exercises
    # vtkPolyData::GetCellArrayInternal across its full 2-bit TARGET domain (the
    # switch->table-lookup optimization); a wrong Target->array mapping would
    # return the wrong points for some cell -> byte divergence.
    n = max(6, size)
    coords = np.empty((n, 3), dtype=dtype)
    idx = np.arange(n, dtype=np.int64)
    coords[:, 0] = (idx % 4).astype(dtype)
    coords[:, 1] = (idx // 4).astype(dtype)
    coords[:, 2] = (idx % 3).astype(dtype)
    pd = vtkPolyData()
    vp = vtkPoints()
    vp.SetData(numpy_to_vtk(np.ascontiguousarray(coords), deep=1))
    pd.SetPoints(vp)

    def _ca(cells):
        ca = vtkCellArray()
        for ids in cells:
            idl = vtkIdList()
            for i in ids:
                idl.InsertNextId(int(i % n))
            ca.InsertNextCell(idl)
        return ca

    pd.SetVerts(_ca([[0], [1]]))
    pd.SetLines(_ca([[0, 1, 2], [3, 4]]))
    pd.SetPolys(_ca([[0, 1, 2], [1, 2, 3, 4]]))
    pd.SetStrips(_ca([[0, 1, 2, 3], [2, 3, 4, 5]]))
    pd.BuildCells()
    cp_flat, sizes, ctypes = [], [], []
    ids = vtkIdList()
    for c in range(pd.GetNumberOfCells()):
        pd.GetCellPoints(c, ids)
        sizes.append(ids.GetNumberOfIds())
        ctypes.append(pd.GetCellType(c))
        for j in range(ids.GetNumberOfIds()):
            cp_flat.append(ids.GetId(j))
    return {
        "cp_flat": np.asarray(cp_flat, dtype=np.int64),
        "sizes": np.asarray(sizes, dtype=np.int64),
        "celltypes": np.asarray(ctypes, dtype=np.int64),
    }


def op_locator_celllocator(dtype, size):
    # vtkCellLocator FindClosestPoint / FindCell / IntersectWithLine over a tet
    # grid -> the devirtualized GetCellBoundsFast / InsideCellBoundsFast bucket
    # walk in Common/DataModel/vtkCellLocator.cxx. Captures the closest points,
    # cell ids, squared distances and line intersections as raw arrays so any
    # byte drift from the devirtualization is caught.
    ug = make_tet_ugrid(size, dtype)
    loc = vtkCellLocator()
    loc.SetDataSet(ug)
    loc.BuildLocator()
    gc = vtkGenericCell()
    lin = np.linspace(0.0, float(size - 1), 5)
    cps, cids, d2s, fcells = [], [], [], []
    for x in lin:
        for y in lin:
            for z in lin:
                p = [float(x), float(y), float(z)]
                cp = [0.0, 0.0, 0.0]
                cellId = reference(0)
                subId = reference(0)
                d2 = reference(0.0)
                loc.FindClosestPoint(p, cp, gc, cellId, subId, d2)
                cps.append(list(cp))
                cids.append(int(cellId))
                d2s.append(float(d2))
                fcells.append(int(loc.FindCell(p)))
    isect_t, isect_x = [], []
    for y in lin:
        for z in lin:
            t = reference(0.0)
            xx = [0.0, 0.0, 0.0]
            pc = [0.0, 0.0, 0.0]
            sub = reference(0)
            hit = loc.IntersectWithLine(
                [-1.0, float(y), float(z)], [float(size), float(y), float(z)],
                1e-6, t, xx, pc, sub,
            )
            isect_t.append(float(t) if hit else -1.0)
            isect_x.append(list(xx) if hit else [0.0, 0.0, 0.0])
    return {
        "closest": np.array(cps, dtype=np.float64),
        "cellids": np.array(cids, dtype=np.int64),
        "d2": np.array(d2s, dtype=np.float64),
        "findcell": np.array(fcells, dtype=np.int64),
        "isect_t": np.array(isect_t, dtype=np.float64),
        "isect_x": np.array(isect_x, dtype=np.float64),
    }


def op_cellcenters(dtype, size):
    # vtkCellCenters over a hex unstructured grid carrying a deterministic cell
    # scalar. RequestData runs the SMP center functor then a second per-cell
    # traversal that calls GetCellType to compact out empty cells -> exercises
    # the empty-cell-mask optimization. The hex grid has no empty cells, so the
    # CopyArrays fast PassData branch is taken and the computed center points
    # (parametric-center EvaluateLocation per cell) must be byte-identical.
    ug = make_hex_ugrid(size, dtype)
    nc = ug.GetNumberOfCells()
    ca = numpy_to_vtk(np.arange(nc, dtype=np.float64), deep=1)
    ca.SetName("cid")
    ug.GetCellData().AddArray(ca)
    cc = vtkCellCenters()
    cc.SetInputData(ug)
    cc.SetCopyArrays(True)
    cc.Update()
    return cc.GetOutput()


def op_cutter(dtype, size):
    # Unstructured hex grid + plane cut with triangle generation OFF -> drives
    # UnstructuredGridCutter -> vtkContourHelper::Contour 3D-cell merge path
    # (the scratch-buffer-reuse optimization).
    p = vtkPlane()
    c = (size - 1) / 2.0
    p.SetOrigin(c, c, c)
    p.SetNormal(1, 1, 0)
    cut = vtkCutter()
    cut.SetInputData(make_hex_ugrid(size, dtype))
    cut.SetCutFunction(p)
    cut.GenerateTrianglesOff()
    cut.SetValue(0, 0.0)
    cut.Update()
    return cut.GetOutput()


def op_cutter_polydata(dtype, size):
    # vtkCutter on a vtkPolyData (triangle sphere) with GenerateTriangles OFF.
    # A polydata input that is NOT eligible for the plane-cutter fast path routes
    # to vtkCutter::DataSetCutter, whose per-cell-point scalar gather
    # (cutScalars/cellScalars are concrete single-component vtkDoubleArrays) is
    # the devirtualized raw-pointer load/store. The default SortBy is
    # SORT_BY_VALUE -> exercises the second gather loop.
    p = vtkPlane()
    p.SetOrigin(0.0, 0.0, 0.0)
    p.SetNormal(1, 1, 1)
    cut = vtkCutter()
    cut.SetInputData(make_sphere(size, size))
    cut.SetCutFunction(p)
    cut.GenerateTrianglesOff()
    cut.SetValue(0, 0.0)
    cut.SetValue(1, 0.25)
    cut.Update()
    return cut.GetOutput()


def op_cutter_polydata_bycell(dtype, size):
    # Same vtkCutter::DataSetCutter path as op_cutter_polydata, but with
    # SortByToSortByCell, which exercises the FIRST per-cell-point scalar gather
    # loop (the SORT_BY_CELL branch) — the other devirtualized raw-pointer copy.
    p = vtkPlane()
    p.SetOrigin(0.0, 0.0, 0.0)
    p.SetNormal(1, 0, 1)
    cut = vtkCutter()
    cut.SetInputData(make_sphere(size, size))
    cut.SetCutFunction(p)
    cut.GenerateTrianglesOff()
    cut.SetSortByToSortByCell()
    cut.SetValue(0, 0.0)
    cut.SetValue(1, 0.25)
    cut.Update()
    return cut.GetOutput()


# ---- vtkCommon operations (explicitly requested) ----
def op_common_dataarray(dtype, size):
    """vtkDataArray / vtkAOSDataArrayTemplate round-trip + tuple/component ops."""
    n = max(16, size * size)
    base = np.ascontiguousarray(
        np.arange(n * 3, dtype=dtype).reshape(n, 3) * dtype(1.5)
    )
    if dtype == np.float64:
        da = vtkDoubleArray()
    else:
        da = vtkFloatArray()
    da.SetNumberOfComponents(3)
    da.SetNumberOfTuples(n)
    for i in range(n):
        da.SetTuple3(i, float(base[i, 0]), float(base[i, 1]), float(base[i, 2]))
    rt = vtk_to_numpy(da).copy()
    # range per component via VTK
    ranges = []
    for c in range(3):
        r = [0.0, 0.0]
        da.GetRange(r, c)
        ranges.extend(r)
    return {"roundtrip": rt, "ranges": np.asarray(ranges, dtype=np.float64)}


def op_common_points(dtype, size):
    """vtkPoints insertion + bounds (the bounds are computed in C++)."""
    pts = make_points_array(max(64, size * size), dtype)
    vp = vtkPoints()
    vp.SetData(numpy_to_vtk(pts, deep=1))
    b = [0.0] * 6
    vp.GetBounds(b)
    return {"data": vtk_to_numpy(vp.GetData()).copy(), "bounds": np.asarray(b)}


def op_common_polydata_links(dtype, size):
    """BuildLinks + GetPointCells / GetCellPoints / GetCellEdgeNeighbors.

    Walks the topology adjacency that fvtk's data-model compiles, capturing the
    full neighbor structure as integer arrays for byte comparison."""
    poly = make_sphere(size, size)
    poly.BuildLinks()
    npts = poly.GetNumberOfPoints()
    ncells = poly.GetNumberOfCells()

    pc_counts = np.empty(npts, dtype=np.int64)
    pc_flat = []
    ids = vtkIdList()
    for p in range(npts):
        poly.GetPointCells(p, ids)
        k = ids.GetNumberOfIds()
        pc_counts[p] = k
        for j in range(k):
            pc_flat.append(ids.GetId(j))

    cp_counts = np.empty(ncells, dtype=np.int64)
    cp_flat = []
    for c in range(ncells):
        poly.GetCellPoints(c, ids)
        k = ids.GetNumberOfIds()
        cp_counts[c] = k
        for j in range(k):
            cp_flat.append(ids.GetId(j))

    # Edge neighbors for the first edge of each cell.
    en_flat = []
    nb = vtkIdList()
    for c in range(ncells):
        poly.GetCellPoints(c, ids)
        if ids.GetNumberOfIds() >= 2:
            p0, p1 = ids.GetId(0), ids.GetId(1)
            poly.GetCellEdgeNeighbors(c, p0, p1, nb)
            en_flat.append(nb.GetNumberOfIds())
            for j in range(nb.GetNumberOfIds()):
                en_flat.append(nb.GetId(j))

    return {
        "pc_counts": pc_counts,
        "pc_flat": np.asarray(pc_flat, dtype=np.int64),
        "cp_counts": cp_counts,
        "cp_flat": np.asarray(cp_flat, dtype=np.int64),
        "edge_neighbors": np.asarray(en_flat, dtype=np.int64),
    }


def op_common_ugrid_build(dtype, size):
    """Construct a vtkUnstructuredGrid by hand from a deterministic point set
    and tetra cells, BuildLinks, and read back topology."""
    n = max(4, size)
    lin = np.linspace(0.0, 1.0, n, dtype=dtype)
    gx, gy, gz = np.meshgrid(lin, lin, lin, indexing="ij")
    pts = np.ascontiguousarray(
        np.stack([gx.ravel(), gy.ravel(), gz.ravel()], axis=1)
    )
    vp = vtkPoints()
    vp.SetData(numpy_to_vtk(pts, deep=1))
    ug = vtkUnstructuredGrid()
    ug.SetPoints(vp)
    # tetra fan over consecutive quadruples
    from vtkmodules.vtkCommonDataModel import VTK_TETRA

    npoints = pts.shape[0]
    ug.Allocate(npoints)
    ids = vtkIdList()
    i = 0
    while i + 3 < npoints:
        ids.Reset()
        for k in range(4):
            ids.InsertNextId(i + k)
        ug.InsertNextCell(VTK_TETRA, ids)
        i += 3
    ug.BuildLinks()
    out = capture_dataobject(ug)
    # also read back via GetCellPoints
    cp = []
    for c in range(ug.GetNumberOfCells()):
        ug.GetCellPoints(c, ids)
        for j in range(ids.GetNumberOfIds()):
            cp.append(ids.GetId(j))
    out["cellpoints"] = np.asarray(cp, dtype=np.int64)
    return out


def op_common_cellarray(dtype, size):
    """vtkCellArray build + offset/connectivity readback."""
    ca = vtkCellArray()
    ids = vtkIdList()
    npoly = max(8, size * 4)
    rng = np.arange(npoly * 3, dtype=np.int64)
    for t in range(npoly):
        ids.Reset()
        ids.InsertNextId(int(rng[3 * t]))
        ids.InsertNextId(int(rng[3 * t + 1]))
        ids.InsertNextId(int(rng[3 * t + 2]))
        ca.InsertNextCell(ids)
    return {
        "conn": np.ascontiguousarray(
            vtk_to_numpy(ca.GetConnectivityArray())
        ).copy(),
        "off": np.ascontiguousarray(vtk_to_numpy(ca.GetOffsetsArray())).copy(),
    }


def op_common_math(dtype, size):
    """vtkMath kernels: cross/dot/norm/determinant/solve over deterministic
    inputs. These are pure C++ scalar kernels — a sharp bit-exactness probe."""
    n = max(32, size * size)
    a = np.ascontiguousarray((np.arange(n * 3) % 17 - 8).astype(np.float64).reshape(n, 3))
    b = np.ascontiguousarray((np.arange(n * 3) % 13 - 6).astype(np.float64).reshape(n, 3))
    cross = np.empty((n, 3))
    dots = np.empty(n)
    norms = np.empty(n)
    for i in range(n):
        c = [0.0, 0.0, 0.0]
        vtkMath.Cross(list(a[i]), list(b[i]), c)
        cross[i] = c
        dots[i] = vtkMath.Dot(list(a[i]), list(b[i]))
        norms[i] = vtkMath.Norm(list(a[i]))
    # 3x3 determinant on a deterministic matrix family
    dets = np.empty(n)
    for i in range(n):
        m0 = list(a[i])
        m1 = list(b[i])
        m2 = [a[i, 0] + b[i, 0], a[i, 1] - b[i, 1], a[i, 2] + 1.0]
        dets[i] = vtkMath.Determinant3x3(m0, m1, m2)
    return {
        "cross": np.ascontiguousarray(cross),
        "dot": dots,
        "norm": norms,
        "det": dets,
    }


def _locator_query(loc_cls, dtype, size):
    pts = make_points_array(max(200, size * size * size), dtype)
    vp = vtkPoints()
    vp.SetData(numpy_to_vtk(pts, deep=1))
    pd = vtkPolyData()
    pd.SetPoints(vp)
    loc = loc_cls()
    loc.SetDataSet(pd)
    loc.BuildLocator()

    # Deterministic query points (lattice within the bounds).
    q = make_points_array(64, np.float64) * 0.97 + 0.001
    closest = np.empty(q.shape[0], dtype=np.int64)
    for i in range(q.shape[0]):
        closest[i] = loc.FindClosestPoint(list(q[i]))

    # FindPointsWithinRadius for a subset.
    within_counts = np.empty(16, dtype=np.int64)
    within_flat = []
    res = vtkIdList()
    for i in range(16):
        loc.FindPointsWithinRadius(0.2, list(q[i]), res)
        within_counts[i] = res.GetNumberOfIds()
        for j in range(res.GetNumberOfIds()):
            within_flat.append(res.GetId(j))
    return {
        "closest": closest,
        "within_counts": within_counts,
        "within_flat": np.asarray(within_flat, dtype=np.int64),
    }


def op_locator_pointlocator(dtype, size):
    return _locator_query(vtkPointLocator, dtype, size)


def op_locator_staticpointlocator(dtype, size):
    return _locator_query(vtkStaticPointLocator, dtype, size)


def op_locator_mergepoints(dtype, size):
    """vtkMergePoints InsertUniquePoint over a set with deliberate duplicates."""
    pts = make_points_array(max(100, size * size), dtype)
    pts = np.concatenate([pts, pts[: len(pts) // 2]], axis=0)  # add duplicates
    vp = vtkPoints()
    pd = vtkPolyData()
    pd.SetPoints(vp)
    mp = vtkMergePoints()
    b = [
        float(pts[:, 0].min()),
        float(pts[:, 0].max()),
        float(pts[:, 1].min()),
        float(pts[:, 1].max()),
        float(pts[:, 2].min()),
        float(pts[:, 2].max()),
    ]
    mp.InitPointInsertion(vp, b)
    assigned = np.empty(pts.shape[0], dtype=np.int64)
    for i in range(pts.shape[0]):
        # IsInsertedPoint returns the existing id, or -1 if new. The
        # bit-exactness probe is that fvtk and stock agree on every dedup verdict
        # and on the resulting merged point coordinates.
        pid = mp.IsInsertedPoint(list(pts[i]))
        if pid < 0:
            pid = mp.InsertNextPoint(list(pts[i]))
        assigned[i] = pid
    return {
        "assigned": assigned,
        "merged_pts": vtk_to_numpy(vp.GetData()).copy(),
    }


def _ply_roundtrip_mesh(dtype, size):
    """Build a triangulated sphere carrying deterministic point coordinates,
    float point normals, and unsigned-char RGB point colors -- the three vertex
    properties the PLY writer gathers per point and the reader scatters back.

    The point array is forced to the requested precision (float32/float64) so the
    writer's per-point coordinate gather exercises BOTH the float and double
    FastDownCast branches; the writer always narrows coordinates to float for the
    PLY 'x/y/z' properties, so the read-back points are float32 either way."""
    s = make_sphere(size, size)
    # Force the point array to the requested precision.
    pts_np = vtk_to_numpy(s.GetPoints().GetData()).astype(dtype)
    pa = numpy_to_vtk(np.ascontiguousarray(pts_np), deep=1)
    newpts = vtkPoints()
    newpts.SetData(pa)
    mesh = vtkPolyData()
    mesh.SetPoints(newpts)
    mesh.SetPolys(s.GetPolys())

    npts = mesh.GetNumberOfPoints()

    # Float point normals (writer requires float normals).
    nfn = vtkPolyDataNormals()
    nfn.SetInputData(mesh)
    nfn.SetComputePointNormals(True)
    nfn.Update()
    normals = vtk_to_numpy(nfn.GetOutput().GetPointData().GetNormals()).astype(
        np.float32
    )
    fa = numpy_to_vtk(np.ascontiguousarray(normals), deep=1)
    fa.SetName("Normals")
    mesh.GetPointData().SetNormals(fa)

    # Deterministic unsigned-char RGB point colors (pure integer arithmetic).
    idx = np.arange(npts, dtype=np.int64)
    rgb = np.empty((npts, 3), dtype=np.uint8)
    rgb[:, 0] = (idx * 7) % 256
    rgb[:, 1] = (idx * 13 + 5) % 256
    rgb[:, 2] = (idx * 29 + 17) % 256
    # uint8 numpy maps to VTK_UNSIGNED_CHAR automatically.
    ca = numpy_to_vtk(np.ascontiguousarray(rgb), deep=1)
    ca.SetName("RGB")
    mesh.GetPointData().SetScalars(ca)
    return mesh


def _ply_roundtrip(dtype, size, file_type):
    """Write `mesh` to a temp .ply (binary or ascii) and read it back, returning
    the read-back vtkPolyData. capture_dataobject then proves the read-back
    points / normals / RGB scalars / face connectivity are byte-identical between
    fvtk and stock VTK -- which only holds if the writer's per-point gather and
    the reader's per-point scatter emit/store identical bytes."""
    mesh = _ply_roundtrip_mesh(dtype, size)

    fd, path = tempfile.mkstemp(suffix=".ply")
    os.close(fd)
    try:
        w = vtkPLYWriter()
        w.SetFileName(path)
        w.SetFileType(file_type)  # 1 = VTK_BINARY, 2 = VTK_ASCII
        w.SetColorModeToDefault()
        w.SetArrayName("RGB")
        # Little-endian binary so the byte layout is fixed across runners.
        w.SetDataByteOrderToLittleEndian()
        w.SetInputData(mesh)
        w.Write()

        r = vtkPLYReader()
        r.SetFileName(path)
        r.Update()
        return capture_dataobject(r.GetOutput())
    finally:
        try:
            os.remove(path)
        except OSError:
            pass


def op_extract_grid(dtype, size):
    """vtkExtractGrid on a curvilinear vtkStructuredGrid with a non-trivial VOI
    AND a non-unit sample rate. This drives vtkExtractStructuredGridHelper::
    CopyPointsAndPointData through BOTH its mapped (downsampling) point-copy path
    and the per-row point-data batch copy. The point array is float32/float64 to
    exercise the typed-pointer copy fast path on both precisions; capture compares
    output points + both point-data arrays + extracted cell extent byte-for-byte."""
    sg = make_structured_grid(size, dtype)
    n = size
    eg = vtkExtractGrid()
    eg.SetInputData(sg)
    # Non-trivial VOI strictly inside the whole extent [0, n-1]^3, asymmetric
    # per-axis so the i/j/k mapping differs across dimensions.
    lo_i, hi_i = 2, n - 2
    lo_j, hi_j = 1, n - 3
    lo_k, hi_k = 3, n - 1
    eg.SetVOI(lo_i, hi_i, lo_j, hi_j, lo_k, hi_k)
    # Non-unit, per-axis sample rate -> exercises the useMapping branch.
    eg.SetSampleRate(2, 3, 2)
    eg.SetIncludeBoundary(1)
    eg.Update()
    return eg.GetOutput()


def op_extract_grid_rowcopy(dtype, size):
    """vtkExtractGrid with a non-trivial VOI but I-sample-rate == 1, so the helper
    takes the canCopyRange path: a contiguous per-row point copy (the std::copy /
    InsertPoints(dstStart,num,srcStart) fast path) with J/K still downsampled
    through the mapping. Complements op_extract_grid (which has I-rate != 1 and so
    takes the per-element mapped path)."""
    sg = make_structured_grid(size, dtype)
    n = size
    eg = vtkExtractGrid()
    eg.SetInputData(sg)
    eg.SetVOI(1, n - 2, 2, n - 1, 1, n - 3)
    eg.SetSampleRate(1, 2, 3)  # I-rate 1 -> canCopyRange contiguous copy
    eg.SetIncludeBoundary(1)
    eg.Update()
    return eg.GetOutput()


def op_ply_roundtrip_binary(dtype, size):
    return _ply_roundtrip(dtype, size, 1)  # VTK_BINARY


def op_ply_roundtrip_ascii(dtype, size):
    return _ply_roundtrip(dtype, size, 2)  # VTK_ASCII


# ===========================================================================
# REGISTRY
# ===========================================================================
# group: "modified" -> hard gate; others -> broad coverage.
# dtypes: which dtype variants to run. sizes: resolution knobs.

OPS = {
    # --- 9 modified filters (HARD GATE) ---
    "decimate": dict(fn=op_decimate, group="modified", dtypes=["float64"], sizes=[24, 48]),
    "smooth": dict(fn=op_smooth, group="modified", dtypes=["float64"], sizes=[24, 48]),
    "normals": dict(fn=op_normals, group="modified", dtypes=["float64"], sizes=[24, 48]),
    "contour": dict(fn=op_contour, group="modified", dtypes=["float32", "float64"], sizes=[20, 32]),
    "clip": dict(fn=op_clip, group="modified", dtypes=["float32", "float64"], sizes=[18, 28]),
    "threshold": dict(fn=op_threshold, group="modified", dtypes=["float32", "float64"], sizes=[20, 32]),
    "warp": dict(fn=op_warp, group="modified", dtypes=["float32", "float64"], sizes=[48, 96]),
    "glyph": dict(fn=op_glyph, group="modified", dtypes=["float64"], sizes=[20, 32]),
    "cell2point": dict(fn=op_cell2point, group="modified", dtypes=["float32", "float64"], sizes=[20, 32]),
    # --- broader filter coverage ---
    "normals_fastpath": dict(fn=op_normals_fastpath, group="filter", dtypes=["float32", "float64"], sizes=[24, 48]),
    "normals_smooth": dict(fn=op_normals_smooth, group="filter", dtypes=["float32", "float64"], sizes=[24, 48]),
    "point2cell": dict(fn=op_point2cell, group="filter", dtypes=["float32", "float64"], sizes=[20, 28]),
    "point2cell_ugrid": dict(fn=op_point2cell_ugrid, group="filter", dtypes=["float32", "float64"], sizes=[8, 12]),
    "elevation": dict(fn=op_elevation, group="filter", dtypes=["float32", "float64"], sizes=[24, 40]),
    "simple_elevation": dict(fn=op_simple_elevation, group="filter", dtypes=["float32", "float64"], sizes=[24, 40]),
    "warpvector": dict(
        fn=op_warpvector, group="filter", dtypes=["float32", "float64"], sizes=[24, 40]
    ),
    "warpscalar_normals": dict(
        fn=op_warpscalar_normals, group="filter", dtypes=["float32", "float64"], sizes=[24, 40]
    ),
    "transform": dict(
        fn=op_transform, group="filter", dtypes=["float32", "float64"], sizes=[24, 40]
    ),
    "transform_nv": dict(
        fn=op_transform_nv, group="filter", dtypes=["float32", "float64"], sizes=[24, 40]
    ),
    "transform_pdf": dict(
        fn=op_transform_pdf, group="filter", dtypes=["float32", "float64"], sizes=[24, 40]
    ),
    "transform_perspective": dict(
        fn=op_transform_perspective, group="filter", dtypes=["float32", "float64"], sizes=[24, 40]
    ),
    "transform_tps": dict(
        fn=op_transform_tps, group="filter", dtypes=["float32", "float64"], sizes=[24, 40]
    ),
    "clean": dict(fn=op_clean, group="filter", dtypes=["float64"], sizes=[24, 40]),
    "triangle": dict(fn=op_triangle, group="filter", dtypes=["float64"], sizes=[24, 40]),
    "geometry": dict(fn=op_geometry, group="filter", dtypes=["float64"], sizes=[18, 28]),
    "shrink": dict(fn=op_shrink, group="filter", dtypes=["float64"], sizes=[16, 24]),
    "connectivity": dict(fn=op_connectivity, group="filter", dtypes=["float64"], sizes=[16, 24]),
    "connectivity_largest": dict(fn=op_connectivity_largest, group="filter", dtypes=["float64"], sizes=[8, 12]),
    "featureedges": dict(fn=op_featureedges, group="filter", dtypes=["float64"], sizes=[24, 40]),
    "stripper": dict(fn=op_stripper, group="filter", dtypes=["float64"], sizes=[24, 40]),
    "vertexglyph": dict(fn=op_vertexglyph, group="filter", dtypes=["float64"], sizes=[24, 40]),
    "glyph_arrays": dict(fn=op_glyph_arrays, group="filter", dtypes=["float64"], sizes=[20, 32]),
    "glyph_mixedcells": dict(fn=op_glyph_mixedcells, group="filter", dtypes=["float64"], sizes=[20, 32]),
    "decimatepro": dict(fn=op_decimatepro, group="filter", dtypes=["float64"], sizes=[24, 40]),
    "cone": dict(fn=op_cone_triangulate, group="filter", dtypes=["float64"], sizes=[12, 30]),
    "contour_triangulator": dict(
        fn=op_contour_triangulator, group="filter", dtypes=["float32", "float64"], sizes=[6, 12]),
    "tube": dict(fn=op_tube, group="filter", dtypes=["float32", "float64"], sizes=[16, 32]),
    "gradient": dict(fn=op_gradient, group="filter", dtypes=["float32", "float64"], sizes=[16, 24]),
    "cutter": dict(fn=op_cutter, group="filter", dtypes=["float64"], sizes=[8, 12]),
    "cutter_polydata": dict(fn=op_cutter_polydata, group="filter", dtypes=["float64"], sizes=[12, 20]),
    "cutter_polydata_bycell": dict(fn=op_cutter_polydata_bycell, group="filter", dtypes=["float64"], sizes=[12, 20]),
    "cellcenters": dict(fn=op_cellcenters, group="filter", dtypes=["float32", "float64"], sizes=[8, 12]),
    "clip_tets": dict(fn=op_clip_tets, group="filter", dtypes=["float32", "float64"], sizes=[6, 10]),
    "contour_hexug": dict(fn=op_contour_hexug, group="filter", dtypes=["float32", "float64"], sizes=[8, 12]),
    "contour_tetug": dict(fn=op_contour_tetug, group="filter", dtypes=["float64"], sizes=[6, 10]),
    "append": dict(fn=op_append, group="filter", dtypes=["float64"], sizes=[16, 28]),
    "append_ugrid_mixed": dict(fn=op_append_ugrid_mixed, group="filter", dtypes=["float32", "float64"], sizes=[5, 8]),
    "append_mixed_types": dict(fn=op_append_mixed_types, group="filter", dtypes=["float32", "float64"], sizes=[5, 8]),
    "append_polydata": dict(fn=op_append_polydata, group="filter", dtypes=["float32", "float64"], sizes=[8, 16]),
    "append_polydata_partial": dict(fn=op_append_polydata_partial, group="filter", dtypes=["float64"], sizes=[8, 16]),
    "probe": dict(fn=op_probe, group="filter", dtypes=["float32", "float64"], sizes=[10, 16]),
    "geometry_ugrid": dict(fn=op_geometry_ugrid, group="filter", dtypes=["float64"], sizes=[8, 14]),
    "threshold_ugrid": dict(fn=op_threshold_ugrid, group="filter", dtypes=["float32", "float64"], sizes=[8, 12]),
    "threshold_ugrid_pointdata": dict(fn=op_threshold_ugrid_pointdata, group="filter", dtypes=["float32", "float64"], sizes=[8, 12]),
    "threshold_ugrid_pointdata_any": dict(fn=op_threshold_ugrid_pointdata_any, group="filter", dtypes=["float32", "float64"], sizes=[8, 12]),
    "threshold_ugrid_celldata": dict(fn=op_threshold_ugrid_celldata, group="filter", dtypes=["float32", "float64"], sizes=[8, 12]),
    "datasetsurface_ugrid": dict(fn=op_datasetsurface_ugrid, group="filter", dtypes=["float32", "float64"], sizes=[8, 12]),
    "tableclip_ugrid": dict(fn=op_tableclip_ugrid, group="filter", dtypes=["float32", "float64"], sizes=[8, 12]),
    "contour_wedgepyr": dict(fn=op_contour_wedgepyr, group="filter", dtypes=["float32", "float64"], sizes=[2, 4]),
    "clip_multicomp": dict(fn=op_clip_multicomp, group="filter", dtypes=["float32", "float64"], sizes=[12, 18]),
    "extract_grid": dict(fn=op_extract_grid, group="filter", dtypes=["float32", "float64"], sizes=[16, 24]),
    "extract_grid_rowcopy": dict(fn=op_extract_grid_rowcopy, group="filter", dtypes=["float32", "float64"], sizes=[16, 24]),
    "locator_celllocator": dict(fn=op_locator_celllocator, group="common", dtypes=["float64"], sizes=[6, 10]),
    "polydata_celltypes": dict(fn=op_polydata_celltypes, group="common", dtypes=["float64"], sizes=[6, 12]),
    # --- vtkCommon ops (explicitly requested) ---
    "common_dataarray": dict(fn=op_common_dataarray, group="common", dtypes=["float32", "float64"], sizes=[8, 16]),
    "common_points": dict(fn=op_common_points, group="common", dtypes=["float32", "float64"], sizes=[8, 16]),
    "common_polydata_links": dict(fn=op_common_polydata_links, group="common", dtypes=["float64"], sizes=[16, 28]),
    "common_ugrid_build": dict(fn=op_common_ugrid_build, group="common", dtypes=["float64"], sizes=[5, 8]),
    "common_cellarray": dict(fn=op_common_cellarray, group="common", dtypes=["float64"], sizes=[8, 32]),
    "common_math": dict(fn=op_common_math, group="common", dtypes=["float64"], sizes=[8, 16]),
    "locator_pointlocator": dict(fn=op_locator_pointlocator, group="common", dtypes=["float64"], sizes=[6, 9]),
    "locator_staticpointlocator": dict(fn=op_locator_staticpointlocator, group="common", dtypes=["float64"], sizes=[6, 9]),
    "locator_mergepoints": dict(fn=op_locator_mergepoints, group="common", dtypes=["float64"], sizes=[10, 16]),
    # --- IO round-trips (PLY writer gather + reader scatter, byte-for-byte) ---
    "ply_roundtrip_binary": dict(fn=op_ply_roundtrip_binary, group="io", dtypes=["float32", "float64"], sizes=[12, 24]),
    "ply_roundtrip_ascii": dict(fn=op_ply_roundtrip_ascii, group="io", dtypes=["float32", "float64"], sizes=[12, 24]),
}

MODIFIED_OPS = {k for k, v in OPS.items() if v["group"] == "modified"}


def iter_cases():
    """Yield (op_name, dtype_name, size) for every parametrized case."""
    for name, spec in OPS.items():
        for dt in spec["dtypes"]:
            for sz in spec["sizes"]:
                yield name, dt, sz


def run_case(op_name, dtype_name, size):
    """Run one case and return {array_name: ndarray}."""
    _require_vtk()
    spec = OPS[op_name]
    dtype = DTYPES[dtype_name]
    result = spec["fn"](dtype, size)
    return _as_arrays(result)
