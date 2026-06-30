#!/usr/bin/env python3
"""cvista wheel post-install smoke test (offscreen / headless).

Imports the package and does a minimal offscreen render to prove the rendering
stack links and runs without a real display (xvfb on Linux, software/native GL
elsewhere).

The fork ships with vtkmodules renamed to the top-level ``cvista`` package, so the
importable namespace is ``cvista`` (e.g. ``from cvista.vtkCommonCore import ...``).
The import name stays parameterizable via CVISTA_IMPORT_NAME for ad-hoc testing of
a differently-named build::

    CVISTA_IMPORT_NAME=cvista python ci/smoke_test.py
"""

import os
import sys

IMPORT_NAME = os.environ.get("CVISTA_IMPORT_NAME", "cvista")


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

    from importlib import import_module

    # The qt helper subpackage must ship: pyvistaqt imports vtkmodules.qt
    # (redirected to cvista.qt) at import time, so a wheel that drops it breaks
    # pyvistaqt (gh-142). It is pure Python and binds no Qt unless one is already
    # imported, so this import is safe with no Qt installed.
    qt = import_module(f"{IMPORT_NAME}.qt")
    assert hasattr(qt, "PyQtImpl"), f"{IMPORT_NAME}.qt missing PyQtImpl"
    print(f"smoke: {IMPORT_NAME}.qt present (PyQtImpl={qt.PyQtImpl})")

    # Offscreen render: prove the OpenGL/render-window stack is importable and
    # can produce a frame headlessly.

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
