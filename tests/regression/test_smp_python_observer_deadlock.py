#!/usr/bin/env python
"""Regression: a threaded SMP backend never runs a Python observer on a worker.

cvista defaults the vtkSMPTools backend to STDThread, so hundreds of filters run
their ``vtkSMPTools::For`` loops multithreaded by default. Several filters report
progress/abort from *inside* the parallel functor using VTK's idiom
``if (vtkSMPTools::GetSingleThread()) this->UpdateProgress(...)``. Under the
(default) Sequential backend the designated thread IS the launcher thread, so the
ProgressEvent observer runs on the main thread -- fine. Under a threaded backend
that designated thread is a *worker*: invoking a Python observer there either
deadlocks (the worker blocks on the GIL the parked launcher holds) or races
(concurrent mutation of Python/pipeline state -> heap corruption). PyVista
attaches such observers pervasively (progress_bar=True), so this hung/crashed its
tests/core.

The fix (main-thread-only events): vtkPythonCommand::Execute returns early when
vtkSMPTools::IsSMPWorkerThread() is true, so a Python observer is NEVER invoked
on a pool worker thread. The launcher thread is never a pool worker, so its
observers still fire; intra-parallel-loop progress ticks are simply skipped
(cosmetic). This changes no filter output.

Two invariants, checked in a *subprocess* (a deadlocked main thread holds the
GIL, so an in-process watchdog cannot fire): the run completes within a generous
timeout (no deadlock/crash), AND every Python observer invocation came from the
main thread (proving no worker ever called into Python).
"""

import subprocess
import sys
import sysconfig
import textwrap

import pytest

# Worst case is a fast machine running this serially; 60s is far above the
# sub-second real runtime but well under any CI step budget, so a hang is
# unambiguous.
TIMEOUT_S = 60

# The child forces STDThread, runs a filter that reports progress from inside its
# SMP functor, and records the thread identity of every observer invocation. With
# the fix the observer is never called on a worker, so all recorded idents equal
# the main-thread ident (and may be zero calls -- all progress was worker-side and
# correctly suppressed). Without the fix it deadlocks or crashes the worker.
_CHILD = textwrap.dedent(
    """
    import sys
    import threading

    from cvista.vtkCommonCore import vtkSMPTools, vtkCommand
    from cvista.vtkImagingCore import vtkRTAnalyticSource
    from cvista.vtkFiltersCore import vtkThreshold

    main_ident = threading.get_ident()

    smp = vtkSMPTools()
    smp.SetBackend("STDThread")
    smp.Initialize(4)
    if smp.GetBackend() != "STDThread":
        print("backend not STDThread:", smp.GetBackend())
        sys.exit(3)

    # A non-trivial number of cells so For() actually dispatches to worker
    # threads (grain < n) instead of running inline on the caller.
    src = vtkRTAnalyticSource()
    src.SetWholeExtent(-40, 40, -40, 40, -40, 40)  # 81^3 points
    src.Update()
    image = src.GetOutput()

    state = {"calls": 0, "offthread": 0}

    def on_progress(obj, evt):
        state["calls"] += 1
        # Calling back into the algorithm mid-execution mimics PyVista's
        # ProgressMonitor (obj.GetProgress()); harmless on the main thread.
        _ = obj.GetProgress()
        if threading.get_ident() != main_ident:
            state["offthread"] += 1

    thr = vtkThreshold()
    thr.SetInputData(image)
    thr.SetInputArrayToProcess(0, 0, 0, 0, "RTData")
    # Wide band keeps (almost) all cells, so output is guaranteed regardless of
    # the default threshold function -- this test cares about threading+progress,
    # not selectivity.
    if hasattr(thr, "SetThresholdFunction") and hasattr(thr, "THRESHOLD_BETWEEN"):
        thr.SetThresholdFunction(thr.THRESHOLD_BETWEEN)
    thr.SetLowerThreshold(-1.0e9)
    thr.SetUpperThreshold(1.0e9)
    thr.AddObserver(vtkCommand.ProgressEvent, on_progress)
    thr.Update()

    out = thr.GetOutput()
    if out.GetNumberOfCells() <= 0:
        print("no output cells")
        sys.exit(4)
    if state["offthread"] != 0:
        print("observer fired on a worker thread %d times" % state["offthread"])
        sys.exit(6)
    print("OK cells=%d progress_calls=%d offthread=%d"
          % (out.GetNumberOfCells(), state["calls"], state["offthread"]))
    sys.exit(0)
    """
)


def test_threaded_smp_never_runs_python_observer_on_worker():
    if sysconfig.get_config_var("Py_GIL_DISABLED"):
        pytest.skip("free-threaded build: no GIL to contend, deadlock cannot occur")

    try:
        proc = subprocess.run(
            [sys.executable, "-c", _CHILD],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_S,
        )
    except subprocess.TimeoutExpired:
        pytest.fail(
            f"threaded SMP + Python ProgressEvent observer deadlocked "
            f"(no completion within {TIMEOUT_S}s) -- worker-thread observer gate missing/broken"
        )

    # rc 6 = observer ran on a worker thread (gate failed); negative rc = the
    # worker crashed the process (the race); both are hard failures.
    assert proc.returncode == 0, (
        f"child failed rc={proc.returncode}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
    )
    assert "OK" in proc.stdout, proc.stdout


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
