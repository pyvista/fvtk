#!/usr/bin/env python
"""Regression: vtkRenderWindowInteractor::Render() releases the Python GIL.

Unlike vtkRenderWindow, the interactor is not a vtkWindow subclass, so it
inherits no wrapping hint: the vtk::unblockthreads hint is declared directly on
vtkRenderWindowInteractor::Render().  The method forwards to the associated
vtkRenderWindow::Render() (which itself releases the GIL) and then fires
RenderEvent, so an interactor-driven render loop must release the GIL just like a
direct vtkRenderWindow::Render() does.  Without the hint a 30 FPS interactor
render thread serializes every other Python thread behind it.

It also checks that a Python observer fired by RenderEvent inside Render()
re-acquires the GIL and runs correctly while another Python thread executes
concurrently.

Requires VTK_PYTHON_FULL_THREADSAFE=ON (cvista's default).  On free-threaded (PEP
703) builds the wrappers never call PyEval_SaveThread, so the probe cannot
discriminate and the test skips.
"""

import sys
import sysconfig
import threading
import time

import pytest

from cvista.vtkFiltersSources import vtkSphereSource
from cvista.vtkRenderingCore import (
    vtkActor,
    vtkPolyDataMapper,
    vtkRenderer,
    vtkRenderWindow,
    vtkRenderWindowInteractor,
)
import cvista.vtkRenderingOpenGL2  # noqa: F401  (registers the OpenGL factory)

# number of Render() calls to probe
N = 30


def test_interactor_render_releases_gil():
    if sysconfig.get_config_var("Py_GIL_DISABLED"):
        pytest.skip("free-threaded build: VTK_PYTHON_FULL_THREADSAFE is compiled out")

    renWin = vtkRenderWindow()
    renWin.SetOffScreenRendering(1)
    renWin.SetSize(300, 300)
    ren = vtkRenderer()
    renWin.AddRenderer(ren)

    sphere = vtkSphereSource()
    sphere.SetThetaResolution(200)
    sphere.SetPhiResolution(200)
    mapper = vtkPolyDataMapper()
    mapper.SetInputConnection(sphere.GetOutputPort())
    actor = vtkActor()
    actor.SetMapper(mapper)
    ren.AddActor(actor)

    iren = vtkRenderWindowInteractor()
    iren.SetRenderWindow(renWin)
    # Enable the interactor so Render() forwards to the render window
    # (vtkRenderWindowInteractor::Render only renders when Enabled and
    # EnableRender are set); do NOT Initialize()/Start(), which would enter the
    # platform event loop.
    iren.EnableRenderOn()
    iren.Enable()

    # warm up outside the probed loop.
    iren.Render()

    events = {"RenderEvent": 0}

    def observer(obj, eventname):
        events[eventname] += 1

    iren.AddObserver("RenderEvent", observer)

    count = [0]
    stopFlag = [False]

    def spin():
        while not stopFlag[0]:
            for _ in range(10000):
                count[0] += 1
            time.sleep(0)

    interval = sys.getswitchinterval()
    thread = threading.Thread(target=spin)
    try:
        # effectively disable preemptive thread switching so that GIL handoffs
        # only happen at explicit release points.
        sys.setswitchinterval(300)
        thread.start()
        base = count[0]
        for _ in range(N):
            iren.Render()
        progressed = count[0] - base
    finally:
        stopFlag[0] = True
        sys.setswitchinterval(interval)
        thread.join()

    assert events["RenderEvent"] == N, "RenderEvent observer did not fire once per Render()"
    assert progressed > 0, "%s.Render() did not release the GIL" % iren.GetClassName()
