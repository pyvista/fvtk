"""Across-thread-count determinism for the fvtk multicore-by-default filters.

fvtk threads a small audited set of bit-exact-safe filters by default (capped at
4 threads); see Common/Core/vtkFVTKSMPDefaults.{h,cxx} and README lever 15. This
test proves the threading is deterministic: it runs the same operations under the
fvtk python at VTK_SMP_MAX_THREADS in {1, 4, 8} and asserts the dumped output is
byte-for-byte identical across all thread counts. Combined with the main
bit-exactness suite (which compares the default 4-thread fvtk against serial stock
VTK 9.6.2), this shows the enabled filters are bit-identical at 1/4/8 threads AND
identical to stock.

Only needs the fvtk python (BITEXACT_FVTK_PY); skips cleanly if unset.
"""
from __future__ import annotations

import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
RUN_OPS = os.path.join(HERE, "run_ops.py")
sys.path.insert(0, HERE)
import compare as _compare  # noqa: E402

# Ops whose filters opt into fvtk default-on threading. Exercising any of these
# at >1 thread must produce byte-identical output to the 1-thread run.
THREADED_OPS = ["warp", "warpvector", "normals", "elevation"]

THREAD_COUNTS = [1, 4, 8]


def _env(ldlp, nthreads):
    env = dict(os.environ)
    if ldlp:
        existing = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = ldlp + (":" + existing if existing else "")
    env["VTK_SMP_MAX_THREADS"] = str(nthreads)
    return env


@pytest.fixture(scope="module")
def thread_runs(tmp_path_factory):
    fvtk_py = os.environ.get("BITEXACT_FVTK_PY")
    if not fvtk_py:
        pytest.skip("BITEXACT_FVTK_PY not set; cannot run SMP determinism test.")
    fvtk_ldlp = os.environ.get("BITEXACT_FVTK_LDLP", "")
    base = str(tmp_path_factory.mktemp("smp_determinism"))
    dirs = {}
    for n in THREAD_COUNTS:
        outdir = os.path.join(base, f"t{n}")
        os.makedirs(outdir, exist_ok=True)
        proc = subprocess.run(
            [fvtk_py, RUN_OPS, outdir],
            env=_env(fvtk_ldlp, n),
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            raise RuntimeError(
                f"fvtk run at {n} threads failed (rc={proc.returncode}):\n"
                f"STDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}"
            )
        dirs[n] = outdir
    return dirs


@pytest.mark.parametrize("nthreads", [n for n in THREAD_COUNTS if n != 1])
@pytest.mark.parametrize("op_name", THREADED_OPS)
def test_threaded_filter_is_thread_count_invariant(thread_runs, op_name, nthreads):
    """Output of a default-threaded filter is byte-identical at 1 vs N threads."""
    ref = thread_runs[1]
    other = thread_runs[nthreads]
    res = _compare.compare_all(ref, other)
    bad = [
        k
        for k, v in res["cases"].items()
        if k.startswith(op_name + "__") and not v["ok"]
    ]
    assert not bad, (
        f"{op_name}: output differs between 1 and {nthreads} threads "
        f"(non-deterministic threading): {bad}"
    )


# A large-mesh script that forces the WARP threaded path: vtkWarpScalar/Vector
# parallelize with grain = vtkSMPTools::THRESHOLD (100k), so the per-suite cases
# (<10k points) run serial regardless of thread count. This drives >THRESHOLD
# points so the multithreaded branch is actually exercised; we hash the output
# points so determinism across thread counts is checked directly.
_LARGE_WARP_SCRIPT = r"""
import os, sys, hashlib
sys.path = [p for p in sys.path if p not in ("", os.getcwd())]
import numpy as np
from vtkmodules.util.numpy_support import numpy_to_vtk, vtk_to_numpy
from vtkmodules.vtkFiltersGeneral import vtkWarpScalar, vtkWarpVector
from vtkmodules.vtkFiltersSources import vtkSphereSource
from vtkmodules.vtkFiltersCore import vtkTriangleFilter, vtkPolyDataNormals
s = vtkSphereSource(); s.SetThetaResolution(700); s.SetPhiResolution(700)
t = vtkTriangleFilter(); t.SetInputConnection(s.GetOutputPort()); t.Update()
nf = vtkPolyDataNormals(); nf.SetInputData(t.GetOutput())
nf.ComputePointNormalsOn(); nf.SplittingOff(); nf.Update()
poly = nf.GetOutput(); n = poly.GetNumberOfPoints()
assert n > 100000, n  # ensure we exceed the warp THRESHOLD grain
poly.GetPointData().SetScalars(numpy_to_vtk(np.linspace(0, 1, n).astype(np.float64), deep=1))
poly.GetPointData().SetVectors(
    numpy_to_vtk(np.ascontiguousarray(np.linspace(0, 1, 3 * n).reshape(n, 3)), deep=1))
ws = vtkWarpScalar(); ws.SetInputData(poly); ws.SetScaleFactor(0.3); ws.Update()
wv = vtkWarpVector(); wv.SetInputData(poly); wv.SetScaleFactor(0.3); wv.Update()
h = hashlib.sha256()
for out in (ws.GetOutput(), wv.GetOutput()):
    a = np.ascontiguousarray(vtk_to_numpy(out.GetPoints().GetData()))
    h.update(a.tobytes())
print(h.hexdigest())
"""


@pytest.fixture(scope="module")
def large_warp_digests(tmp_path_factory):
    fvtk_py = os.environ.get("BITEXACT_FVTK_PY")
    if not fvtk_py:
        pytest.skip("BITEXACT_FVTK_PY not set.")
    script = tmp_path_factory.mktemp("large_warp") / "large_warp.py"
    script.write_text(_LARGE_WARP_SCRIPT)
    digests = {}
    for n in THREAD_COUNTS:
        proc = subprocess.run(
            [fvtk_py, str(script)],
            env=_env(os.environ.get("BITEXACT_FVTK_LDLP", ""), n),
            capture_output=True,
            text=True,
        )
        assert proc.returncode == 0, proc.stderr
        digests[n] = proc.stdout.strip().splitlines()[-1]
    return digests


@pytest.mark.parametrize("nthreads", [n for n in THREAD_COUNTS if n != 1])
def test_large_warp_threaded_path_is_deterministic(large_warp_digests, nthreads):
    """The >THRESHOLD warp path (genuinely multithreaded) hashes identically at
    1 vs N threads."""
    ref = large_warp_digests[1]
    got = large_warp_digests[nthreads]
    assert got == ref, (
        f"large warp output digest at {nthreads} threads ({got[:16]}) != "
        f"1-thread reference ({ref[:16]}) -- threading is non-deterministic"
    )


# A large-mesh script that forces the vtkLinearTransform threaded path:
# TransformPoints/TransformNormals parallelize with grain = vtkSMPTools::THRESHOLD
# (100k), so per-suite cases (<10k points) run serial regardless of thread count.
# This drives >THRESHOLD points so the multithreaded transform branch is actually
# exercised, applying a non-trivial rotate+scale+translate and carrying normals
# through (TransformAllInputVectorsOn). We hash the transformed points AND normals
# so determinism across thread counts is checked directly.
_LARGE_TRANSFORM_SCRIPT = r"""
import os, sys, hashlib
sys.path = [p for p in sys.path if p not in ("", os.getcwd())]
import numpy as np
from vtkmodules.util.numpy_support import vtk_to_numpy
from vtkmodules.vtkCommonTransforms import vtkTransform
from vtkmodules.vtkFiltersGeneral import vtkTransformFilter
from vtkmodules.vtkFiltersSources import vtkSphereSource
from vtkmodules.vtkFiltersCore import vtkTriangleFilter, vtkPolyDataNormals
s = vtkSphereSource(); s.SetThetaResolution(700); s.SetPhiResolution(700)
t = vtkTriangleFilter(); t.SetInputConnection(s.GetOutputPort()); t.Update()
nf = vtkPolyDataNormals(); nf.SetInputData(t.GetOutput())
nf.ComputePointNormalsOn(); nf.SplittingOff(); nf.Update()
poly = nf.GetOutput(); n = poly.GetNumberOfPoints()
assert n > 100000, n  # ensure we exceed the transform THRESHOLD grain
xf = vtkTransform()
xf.Translate(1.25, -3.5, 7.0)
xf.RotateWXYZ(37.0, 0.3, 0.6, 0.8)
xf.Scale(1.7, 0.4, 2.3)
tf = vtkTransformFilter()
tf.SetTransform(xf)
tf.TransformAllInputVectorsOn()
tf.SetInputData(poly)
tf.Update()
out = tf.GetOutput()
h = hashlib.sha256()
h.update(np.ascontiguousarray(vtk_to_numpy(out.GetPoints().GetData())).tobytes())
h.update(np.ascontiguousarray(vtk_to_numpy(out.GetPointData().GetNormals())).tobytes())
print(h.hexdigest())
"""


@pytest.fixture(scope="module")
def large_transform_digests(tmp_path_factory):
    fvtk_py = os.environ.get("BITEXACT_FVTK_PY")
    if not fvtk_py:
        pytest.skip("BITEXACT_FVTK_PY not set.")
    script = tmp_path_factory.mktemp("large_transform") / "large_transform.py"
    script.write_text(_LARGE_TRANSFORM_SCRIPT)
    digests = {}
    for n in THREAD_COUNTS:
        proc = subprocess.run(
            [fvtk_py, str(script)],
            env=_env(os.environ.get("BITEXACT_FVTK_LDLP", ""), n),
            capture_output=True,
            text=True,
        )
        assert proc.returncode == 0, proc.stderr
        digests[n] = proc.stdout.strip().splitlines()[-1]
    return digests


@pytest.mark.parametrize("nthreads", [n for n in THREAD_COUNTS if n != 1])
def test_large_transform_threaded_path_is_deterministic(
    large_transform_digests, nthreads
):
    """The >THRESHOLD vtkLinearTransform path (genuinely multithreaded) hashes
    identically at 1 vs N threads."""
    ref = large_transform_digests[1]
    got = large_transform_digests[nthreads]
    assert got == ref, (
        f"large transform output digest at {nthreads} threads ({got[:16]}) != "
        f"1-thread reference ({ref[:16]}) -- threading is non-deterministic"
    )
