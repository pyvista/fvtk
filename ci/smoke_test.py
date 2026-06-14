#!/usr/bin/env python3
"""fvtk wheel post-install smoke test (offscreen / headless).

Imports the package and does a minimal offscreen render to prove the rendering
stack links and runs without a real display (xvfb on Linux, software/native GL
elsewhere).

The import name is parameterizable for the in-flight vtkmodules -> fvtk package
rename (branch feat/fvtk-namespace). Override with FVTK_IMPORT_NAME, e.g.::

    FVTK_IMPORT_NAME=fvtk python ci/smoke_test.py

TODO(namespace): default FVTK_IMPORT_NAME to "fvtk" once the rename merges to
main; until then the current package namespace is "vtkmodules".
"""

import os
import sys

IMPORT_NAME = os.environ.get("FVTK_IMPORT_NAME", "vtkmodules")


def main() -> int:
    print(f"smoke: importing top-level package '{IMPORT_NAME}'")
    pkg = __import__(IMPORT_NAME)
    print(f"smoke: {IMPORT_NAME} -> {getattr(pkg, '__file__', '<namespace>')}")

    # Version banner (vtkmodules exposes VTK_VERSION via vtkCommonCore).
    try:
        from importlib import import_module

        cc = import_module(f"{IMPORT_NAME}.vtkCommonCore")
        print(f"smoke: VTK_VERSION = {cc.vtkVersion.GetVTKVersion()}")
    except Exception as exc:  # noqa: BLE001
        print(f"smoke: WARNING could not read VTK version: {exc}")

    # Offscreen render: prove the OpenGL/render-window stack is importable and
    # can produce a frame headlessly.
    from importlib import import_module

    # autoinit wires the rendering factory overrides.
    import_module(f"{IMPORT_NAME}.vtkRenderingOpenGL2")
    import_module(f"{IMPORT_NAME}.vtkInteractionStyle")
    src_mod = import_module(f"{IMPORT_NAME}.vtkFiltersSources")
    ren_mod = import_module(f"{IMPORT_NAME}.vtkRenderingCore")

    sphere = src_mod.vtkSphereSource()
    mapper = ren_mod.vtkPolyDataMapper()
    mapper.SetInputConnection(sphere.GetOutputPort())
    actor = ren_mod.vtkActor()
    actor.SetMapper(mapper)

    renderer = ren_mod.vtkRenderer()
    render_window = ren_mod.vtkRenderWindow()
    render_window.SetOffScreenRendering(1)
    render_window.AddRenderer(renderer)
    renderer.AddActor(actor)
    render_window.SetSize(300, 300)
    render_window.Render()
    print("smoke: offscreen Render() OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
