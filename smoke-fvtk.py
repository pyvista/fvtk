#!/usr/bin/env python3
"""Native fvtk smoke test — validates the `fvtk` package (post vtkmodules rename).

fvtk installs as the `fvtk` import package with dist name `fvtk`, so the stock-PyVista
parity suite (which imports `vtkmodules`) cannot drive it unpatched. This exercises the
renamed package directly: top-level import, the kit submodules, the util/numpy_interface
shims, a numpy<->vtk roundtrip, a filter pipeline, and an EGL offscreen render.

Run inside the build's nix-shell so EGL/OSMesa resolve, e.g.:

    pip install dist/fvtk-*.whl
    VTK_DEFAULT_OPENGL_WINDOW=vtkEGLRenderWindow VTK_EGL_DEVICE_INDEX=0 \
        python smoke-fvtk.py

Exit code 0 = all pass.
"""
import importlib.util
import os
import sys

FAILS = []


def check(name, fn):
    try:
        fn()
        print(f"  ok   {name}", flush=True)
    except Exception as e:  # noqa: BLE001
        print(f"  FAIL {name}: {type(e).__name__}: {e}", flush=True)
        FAILS.append(name)


print("=== fvtk namespace smoke ===", flush=True)

def _toplevel():
    import fvtk  # noqa: F401
    assert importlib.util.find_spec("vtkmodules") is None, "stray vtkmodules present"
check("import fvtk (no stray vtkmodules)", _toplevel)

def _version():
    from fvtk.vtkCommonCore import vtkVersion
    print("       VTK_VERSION =", vtkVersion.GetVTKVersion(), flush=True)
check("fvtk.vtkCommonCore.vtkVersion", _version)

def _kits():
    from fvtk import (  # noqa: F401
        vtkCommonCore, vtkCommonDataModel, vtkCommonExecutionModel,
        vtkFiltersCore, vtkFiltersGeometry, vtkRenderingCore,
    )
check("from fvtk import <kits>", _kits)

def _instantiate():
    from fvtk.vtkCommonDataModel import vtkPolyData
    mod = type(vtkPolyData()).__module__
    assert mod.startswith("fvtk") and "vtkmodules" not in mod, f"type module: {mod}"
check("instantiate vtkPolyData (fvtk.* module)", _instantiate)

def _util():
    import numpy as np
    from fvtk.util.numpy_support import numpy_to_vtk, vtk_to_numpy
    a = np.arange(30, dtype=np.float64).reshape(10, 3)
    assert np.allclose(a, vtk_to_numpy(numpy_to_vtk(a))), "numpy roundtrip mismatch"
check("fvtk.util.numpy_support roundtrip", _util)

def _dsa():
    from fvtk.numpy_interface import dataset_adapter as dsa  # noqa: F401
check("fvtk.numpy_interface.dataset_adapter", _dsa)

def _filter():
    from fvtk.vtkFiltersSources import vtkSphereSource
    from fvtk.vtkFiltersCore import vtkTriangleFilter
    s = vtkSphereSource(); s.SetThetaResolution(16); s.SetPhiResolution(16)
    t = vtkTriangleFilter(); t.SetInputConnection(s.GetOutputPort()); t.Update()
    n = t.GetOutput().GetNumberOfCells()
    assert n > 0, "no cells out of sphere->triangle pipeline"
    print("       sphere->triangle cells =", n, flush=True)
check("filter pipeline (sphere->triangle)", _filter)

def _render():
    os.environ.setdefault("VTK_DEFAULT_OPENGL_WINDOW", "vtkEGLRenderWindow")
    os.environ.setdefault("VTK_EGL_DEVICE_INDEX", "0")
    from fvtk.vtkFiltersSources import vtkConeSource
    from fvtk.vtkRenderingCore import (
        vtkRenderer, vtkRenderWindow, vtkPolyDataMapper, vtkActor)
    import fvtk.vtkRenderingOpenGL2  # noqa: F401  (registers GL factory)
    cone = vtkConeSource()
    m = vtkPolyDataMapper(); m.SetInputConnection(cone.GetOutputPort())
    a = vtkActor(); a.SetMapper(m)
    ren = vtkRenderer(); ren.AddActor(a)
    rw = vtkRenderWindow(); rw.SetOffScreenRendering(1); rw.AddRenderer(ren)
    rw.SetSize(150, 150); rw.Render()
    print("       offscreen render OK; window =", rw.GetClassName(), flush=True)
check("EGL offscreen render", _render)

print(f"=== {'ALL PASS' if not FAILS else 'FAILURES: ' + ', '.join(FAILS)} ===", flush=True)
sys.exit(1 if FAILS else 0)
