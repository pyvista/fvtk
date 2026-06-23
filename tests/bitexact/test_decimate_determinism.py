"""Thread-count determinism for the fvtk opt-in parallel decimation kernel.

The EnableFast decimation path (Filters/Core/fvtkFastDecimate.cxx) is a parallel
MIS-wave half-edge-collapse decimator. Its output is ORDER-RELAXED (points are a
subset of the input, renumbered in a thread-dependent order; cells likewise),
but it MUST be thread-count INVARIANT: the same point SET and the same triangle
MULTISET regardless of how many threads ran. That invariance is what the
distance-2 MIS + deterministic (error, id) tie-break buy us; this test proves it
by running the kernel under FVTK_FAST=1 at VTK_SMP_MAX_THREADS in {1, 4, 8} and
asserting the order-invariant mesh is identical across all thread counts.

Needs only the fvtk python (BITEXACT_FVTK_PY); skips cleanly if unset.
"""
from __future__ import annotations

import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))

THREAD_COUNTS = [1, 4, 8]

# Drives the fast decimation kernel and prints a canonical, ORDER-INVARIANT
# digest of the output: a sorted hash of the point set + a sorted hash of the
# triangle multiset (each triangle's three coordinates sorted, then the triangle
# list sorted). Two runs with the same mesh (any order/renumbering) hash equal.
_SCRIPT = r"""
import os, sys, hashlib
sys.path = [p for p in sys.path if p not in ("", os.getcwd())]
import numpy as np
from fvtk.vtkCommonDataModel import vtkPolyData
from fvtk.vtkFiltersCore import vtkDecimatePro, vtkTriangleFilter
from fvtk.vtkFiltersSources import vtkSphereSource
from fvtk.util.numpy_support import vtk_to_numpy

os.environ["FVTK_FAST"] = "1"

s = vtkSphereSource(); s.SetThetaResolution(120); s.SetPhiResolution(120)
tf = vtkTriangleFilter(); tf.SetInputConnection(s.GetOutputPort()); tf.Update()
poly = vtkPolyData(); poly.DeepCopy(tf.GetOutput())

before = vtkDecimatePro.GetFastModeEngageCount()
d = vtkDecimatePro(); d.SetInputData(poly); d.SetTargetReduction(0.6)
d.PreserveTopologyOff(); d.BoundaryVertexDeletionOn(); d.Update()
after = vtkDecimatePro.GetFastModeEngageCount()
assert after > before, "fast decimation path did not engage"

out = d.GetOutput()
pts = np.ascontiguousarray(vtk_to_numpy(out.GetPoints().GetData()))
polys = out.GetPolys()
conn = np.asarray(vtk_to_numpy(polys.GetConnectivityArray())).reshape(-1, 3)

# ORDER-INVARIANT point-set digest: sort point rows lexicographically by bytes.
ph = hashlib.sha256()
order = np.lexsort((pts[:, 2], pts[:, 1], pts[:, 0]))
ph.update(np.ascontiguousarray(pts[order]).tobytes())

# ORDER-INVARIANT triangle-multiset digest: map each connectivity id to its
# point coordinate, sort the 3 vertices within each triangle, then sort the
# triangle list. Independent of point renumbering AND triangle order.
tri_coords = pts[conn]                      # (ntri, 3, 3)
tri_coords = np.sort(tri_coords.reshape(tri_coords.shape[0], -1), axis=1)  # sort within
tri_order = np.lexsort(tri_coords.T[::-1])
th = hashlib.sha256()
th.update(np.ascontiguousarray(tri_coords[tri_order]).tobytes())

print(ph.hexdigest())
print(th.hexdigest())
print(out.GetNumberOfPoints(), out.GetNumberOfPolys())
"""


def _env(ldlp, nthreads):
    env = dict(os.environ)
    if ldlp:
        existing = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = ldlp + (":" + existing if existing else "")
    env["VTK_SMP_MAX_THREADS"] = str(nthreads)
    env["FVTK_FAST"] = "1"
    return env


@pytest.fixture(scope="module")
def decimate_digests(tmp_path_factory):
    fvtk_py = os.environ.get("BITEXACT_FVTK_PY")
    if not fvtk_py:
        pytest.skip("BITEXACT_FVTK_PY not set; cannot run decimate determinism test.")
    fvtk_ldlp = os.environ.get("BITEXACT_FVTK_LDLP", "")
    script = tmp_path_factory.mktemp("decimate_det") / "decimate.py"
    script.write_text(_SCRIPT)
    digests = {}
    for n in THREAD_COUNTS:
        proc = subprocess.run(
            [fvtk_py, str(script)],
            env=_env(fvtk_ldlp, n),
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            raise RuntimeError(
                f"fvtk fast decimation at {n} threads failed (rc={proc.returncode}):\n"
                f"STDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}"
            )
        lines = proc.stdout.strip().splitlines()
        # last three lines: point-digest, triangle-digest, "npts npolys"
        digests[n] = (lines[-3], lines[-2], lines[-1])
    return digests


@pytest.mark.parametrize("nthreads", [n for n in THREAD_COUNTS if n != 1])
def test_decimate_is_thread_count_invariant(decimate_digests, nthreads):
    """The fast decimation output (order-relaxed) is the SAME mesh -- same point
    set and same triangle multiset -- at 1 vs N threads."""
    ref = decimate_digests[1]
    got = decimate_digests[nthreads]
    assert got[0] == ref[0], (
        f"point set differs between 1 and {nthreads} threads "
        f"({got[0][:16]} != {ref[0][:16]})"
    )
    assert got[1] == ref[1], (
        f"triangle multiset differs between 1 and {nthreads} threads "
        f"({got[1][:16]} != {ref[1][:16]})"
    )
    assert got[2] == ref[2], (
        f"output cardinality differs between 1 and {nthreads} threads "
        f"({got[2]} != {ref[2]})"
    )
