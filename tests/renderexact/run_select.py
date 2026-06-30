"""Standalone driver: hardware-selection (picking) parity for cvista vs stock VTK.

Sister to ``run_render.py``. Where that gate proves pixels match, this one proves
the GPU **hardware-selection** path maps picked pixels back to the SAME original
point ids on both backends -- the path that ``run_render.py`` never exercises.

Why this gate exists: cvista stores the ``vtkOriginalPointIds`` passthrough array of
``vtkGeometryFilter`` in an int32 container (width-relaxed) when the ids fit,
where stock VTK uses int64. The render hardware-selector remaps a picked pixel's
raw VTK point id to the value in that array. If a mapper fetched the array with a
width-specific ``vtkArrayDownCast<vtkIdTypeArray>`` (null on int32), the remap
would be silently skipped and picking would return the WRONG ids -- a break that
neither the bit-exact nor the pixel-exact gate can see. The cvista mappers were
reworked to read the array width-agnostically; this driver is the gate for that.

Scene design mirrors run_render.py's determinism rules (fixed offscreen EGL
window, fixed camera, deterministic integer-algebra geometry, no MSAA), so both
backends drive an identical GL command stream and the only thing that can differ
is the id-array read. The scene extracts the surface of a hex lattice so the
surviving surface points are a NON-IDENTITY subset of the input points -- i.e.
``vtkOriginalPointIds[surfaceId] != surfaceId`` -- which makes the test
discriminating: a dropped int32 remap yields surface ids, not original ids.

Usage:  python run_select.py <output_dir> [scene_filter]
"""
from __future__ import annotations

import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import render_ops  # noqa: E402  (reuse _renderer/_fixed_camera/_new_window)

from vtkmodules.vtkCommonCore import vtkPoints  # noqa: E402
from vtkmodules.vtkCommonDataModel import (  # noqa: E402
    vtkUnstructuredGrid,
    vtkDataObject,
    VTK_HEXAHEDRON,
)
from vtkmodules.vtkFiltersGeometry import vtkGeometryFilter  # noqa: E402
from vtkmodules.vtkRenderingCore import (  # noqa: E402
    vtkActor,
    vtkPolyDataMapper,
    vtkHardwareSelector,
)
from vtkmodules.util.numpy_support import vtk_to_numpy  # noqa: E402

# Register the GL backend factory overrides (same as run_render.py).
import vtkmodules.vtkRenderingOpenGL2  # noqa: F401,E402


def _hex_lattice(n):
    """An n x n x n point lattice meshed with (n-1)^3 hexahedra. The surface
    extraction will drop the interior points, so vtkOriginalPointIds becomes a
    non-identity map (the property this gate relies on)."""
    grid = vtkUnstructuredGrid()
    pts = vtkPoints()
    for k in range(n):
        for j in range(n):
            for i in range(n):
                # centered, unit-ish extent; pure integer algebra -> identical
                # coordinates on both backends.
                pts.InsertNextPoint(
                    (i - (n - 1) / 2.0) * 0.4,
                    (j - (n - 1) / 2.0) * 0.4,
                    (k - (n - 1) / 2.0) * 0.4,
                )
    grid.SetPoints(pts)

    def pid(i, j, k):
        return (k * n + j) * n + i

    grid.Allocate((n - 1) ** 3)
    for k in range(n - 1):
        for j in range(n - 1):
            for i in range(n - 1):
                ids = [
                    pid(i, j, k),
                    pid(i + 1, j, k),
                    pid(i + 1, j + 1, k),
                    pid(i, j + 1, k),
                    pid(i, j, k + 1),
                    pid(i + 1, j, k + 1),
                    pid(i + 1, j + 1, k + 1),
                    pid(i, j + 1, k + 1),
                ]
                grid.InsertNextCell(VTK_HEXAHEDRON, 8, ids)
    return grid


def scene_surface_pointpick():
    """Surface of a hex lattice, with the mapper wired to remap picked point ids
    through the vtkOriginalPointIds passthrough array."""
    grid = _hex_lattice(6)
    geom = vtkGeometryFilter()
    geom.SetInputData(grid)
    geom.PassThroughPointIdsOn()
    geom.PassThroughCellIdsOn()
    geom.Update()
    surf = geom.GetOutput()

    m = vtkPolyDataMapper()
    m.SetInputData(surf)
    m.SetPointIdArrayName("vtkOriginalPointIds")
    m.SetCellIdArrayName("vtkOriginalCellIds")
    a = vtkActor()
    a.SetMapper(m)
    a.GetProperty().SetPointSize(3.0)

    ren = render_ops._renderer()
    ren.AddActor(a)
    render_ops._fixed_camera(ren, dist=3.0)
    rw = render_ops._new_window(ren)
    return ren, rw, surf


SCENES = {
    "surface_pointpick": scene_surface_pointpick,
}


def _select_point_ids(ren, rw):
    """Run a full-viewport hardware POINT selection; return the sorted-unique set
    of selected ids (these are the REMAPPED original ids because the mapper has a
    PointIdArrayName set)."""
    w, h = rw.GetSize()
    sel = vtkHardwareSelector()
    sel.SetRenderer(ren)
    sel.SetArea(0, 0, w - 1, h - 1)
    sel.SetFieldAssociation(vtkDataObject.FIELD_ASSOCIATION_POINTS)
    result = sel.Select()
    ids = []
    n_nodes = result.GetNumberOfNodes()
    for i in range(n_nodes):
        node = result.GetNode(i)
        sl = node.GetSelectionList()
        if sl is not None and sl.GetNumberOfTuples() > 0:
            ids.append(np.asarray(vtk_to_numpy(sl)).astype(np.int64).ravel())
    if ids:
        allids = np.concatenate(ids)
    else:
        allids = np.empty(0, dtype=np.int64)
    return np.unique(allids), n_nodes


def main():
    out = sys.argv[1]
    scene_filter = sys.argv[2] if len(sys.argv) > 2 else None
    os.makedirs(out, exist_ok=True)

    from vtkmodules.vtkCommonCore import vtkVersion
    import vtkmodules

    manifest = {"cases": {}}
    n_ok = n_err = 0

    for name, fn in SCENES.items():
        if scene_filter and scene_filter not in name:
            continue
        try:
            ren, rw, surf = fn()
            rw.Render()
            ids, n_nodes = _select_point_ids(ren, rw)
            orig = surf.GetPointData().GetArray("vtkOriginalPointIds")
            orig_dtype = str(vtk_to_numpy(orig).dtype) if orig is not None else "MISSING"
            np.savez(os.path.join(out, name + ".npz"), selected_point_ids=ids)
            manifest["cases"][name] = {
                "scene": name,
                "n_selected": int(ids.size),
                "n_nodes": int(n_nodes),
                "orig_pointids_dtype": orig_dtype,
                "selected_min": int(ids.min()) if ids.size else -1,
                "selected_max": int(ids.max()) if ids.size else -1,
            }
            rw.Finalize()
            n_ok += 1
        except Exception as e:  # noqa: BLE001
            manifest["cases"][name] = {"scene": name, "error": repr(e)}
            n_err += 1
            print(f"ERROR {name}: {e!r}", file=sys.stderr)

    manifest["provenance"] = {
        "numpy": np.__version__,
        "vtk_version": vtkVersion.GetVTKVersion(),
        "vtkmodules_file": getattr(vtkmodules, "__file__", "?"),
    }
    with open(os.path.join(out, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=2, sort_keys=True)

    print(
        f"[run_select] backend={manifest['provenance']['vtkmodules_file']} "
        f"vtk={manifest['provenance']['vtk_version']}"
    )
    print(f"[run_select] wrote {n_ok} scenes, {n_err} errors -> {out}")
    return 1 if n_err else 0


if __name__ == "__main__":
    sys.exit(main())
