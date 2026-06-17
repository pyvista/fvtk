#!/usr/bin/env python3
"""fvtk wheel post-install smoke test (minimal, cross-platform, NO render).

The offscreen-render smoke (ci/smoke_test.py) is Linux-only: it needs xvfb plus
the OSMesa/EGL/GLX rendering stack. macOS-arm64 and Windows wheels ship a native
GL backend (Cocoa / WGL) with no headless display, so a render smoke would fail
on the CI runners. This minimal smoke instead proves the wheel imports and that
a non-rendering filter pipeline actually computes — enough to certify the build,
auditwheel/delocate/delvewheel repair, and the C++ <-> Python wrapper bridge.

  import fvtk -> print VTK version -> vtkSphereSource -> vtkTriangleFilter,
  assert the output has > 0 cells.

The import name stays parameterizable via FVTK_IMPORT_NAME (the fork renames
vtkmodules to the top-level ``fvtk`` package)::

    FVTK_IMPORT_NAME=fvtk python ci/smoke_min.py
"""

import os
import sys
from importlib import import_module

IMPORT_NAME = os.environ.get("FVTK_IMPORT_NAME", "fvtk")


def main() -> int:
    print(f"smoke_min: importing top-level package '{IMPORT_NAME}'")
    pkg = __import__(IMPORT_NAME)
    print(f"smoke_min: {IMPORT_NAME} -> {getattr(pkg, '__file__', '<namespace>')}")

    cc = import_module(f"{IMPORT_NAME}.vtkCommonCore")
    print(f"smoke_min: VTK_VERSION = {cc.vtkVersion.GetVTKVersion()}")

    # Non-rendering filter pipeline: sphere -> triangulate. Exercises the
    # Filters/Sources + Filters/Core kits and the Python wrapper bridge without
    # touching OpenGL.
    src_mod = import_module(f"{IMPORT_NAME}.vtkFiltersSources")
    core_mod = import_module(f"{IMPORT_NAME}.vtkFiltersCore")

    sphere = src_mod.vtkSphereSource()
    sphere.SetThetaResolution(16)
    sphere.SetPhiResolution(16)

    tri = core_mod.vtkTriangleFilter()
    tri.SetInputConnection(sphere.GetOutputPort())
    tri.Update()

    n_cells = tri.GetOutput().GetNumberOfCells()
    print(f"smoke_min: vtkTriangleFilter output cells = {n_cells}")
    if n_cells <= 0:
        print("smoke_min: FAIL — filter produced no cells", file=sys.stderr)
        return 1

    print("smoke_min: import + compute OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
