#!/usr/bin/env python
"""Regression: vtkRenderWindow::Render() releases the Python GIL.

The vtk::unblockthreads hint is declared on vtkWindow::Render() and
vtkRenderWindow::Render(), and the wrapper generator propagates the hint to the
Render() overrides of the concrete render window classes, so the
factory-instantiated window must release the GIL while the C++ render executes.
A spinner thread can only make progress while the main thread has dropped the
GIL, so after a burst of renders it must have advanced.

It also checks that Python observers fired by events inside Render() (StartEvent,
EndEvent, RenderEvent) re-acquire the GIL and run correctly while another Python
thread executes concurrently.

Requires VTK_PYTHON_FULL_THREADSAFE=ON (cvista's default). On free-threaded (PEP
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
)
import cvista.vtkRenderingOpenGL2  # noqa: F401  (registers the OpenGL factory)

# number of Render() calls to probe
N = 30


def test_renderwindow_render_releases_gil():
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

    # warm up outside the probed loop: context creation, shader compilation,
    # pipeline update, VBO upload.
    renWin.Render()

    # observers fired inside Render() run under PyGILState_Ensure
    # (vtkPythonCommand), which must be safe while the wrapper has released the
    # GIL around the C++ call.
    events = {"StartEvent": 0, "EndEvent": 0, "RenderEvent": 0}

    def observer(obj, eventname):
        events[eventname] += 1

    for name in events:
        renWin.AddObserver(name, observer)

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
            renWin.Render()
        progressed = count[0] - base
    finally:
        stopFlag[0] = True
        sys.setswitchinterval(interval)
        thread.join()

    assert events["RenderEvent"] == N, "RenderEvent observer did not fire once per Render()"
    assert progressed > 0, "%s.Render() did not release the GIL" % renWin.GetClassName()
