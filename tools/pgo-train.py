#!/usr/bin/env python3
"""PGO training workload — runs against the instrumented cvista to record profiles.

Goal: exercise the breadth of PyVista's CPU hot paths in REALISTIC proportion, so
the profile reflects how the filter stack is actually used. Each representative
operation runs a moderate number of times on varied data; no single path is
over-weighted. (An earlier contour-skewed variant was net-negative: over-weighting
the marching-cubes path shifted PGO's layout budget off the shared cutter/slice
path and cost more than it saved. The contour ops carry a small, inherent PGO
regression that training skew does not fix — so train representatively instead.)

DELIBERATELY GL-FREE: no rendering. Every measured PGO win is in CPU-bound filters;
the offscreen-render path is GL-bound and shows ~no PGO benefit. Keeping training
headless-GL-independent makes it robust in CI (where EGL/OSMesa is finicky) — and
critically, a render segfault would bypass gcov's atexit dump and lose the ENTIRE
profile for that process. Filters only => clean exit => profiles always written.

Profiles (.gcda) merge across every instrumented process exit.
"""
import os, sys
_selfdir = os.path.dirname(os.path.abspath(__file__))
sys.path = [p for p in sys.path if p not in ("", _selfdir)]
import numpy as np
import pyvista as pv
pv.OFF_SCREEN = True

# --- fixtures (representative sizes) -----------------------------------------
vol = pv.ImageData(dimensions=(80, 80, 80))
vol.point_data["v"] = np.linalg.norm(
    np.indices((80, 80, 80)).reshape(3, -1).T - 40, axis=1).astype(np.float64)

res = 300
grid = pv.ImageData(dimensions=(res, res, 1))
gx = np.linspace(0, 4 * np.pi, res)
xv, yv = np.meshgrid(gx, gx)
grid.point_data["s"] = (np.sin(xv) * np.cos(yv)).ravel().astype(np.float64)

sphere = pv.Sphere(theta_resolution=200, phi_resolution=200)
sphere.point_data["g"] = np.ones(sphere.n_points)
arrow = pv.Arrow()

# --- balanced CPU hot-path coverage: each op a handful of times --------------
N = 4
for _ in range(N):
    # array marshalling
    g = grid.copy(deep=False); g.point_data["s2"] = grid.point_data["s"]; _ = np.asarray(g.point_data["s2"]).sum()
    # contouring (normal weight — not skewed)
    grid.contour(isosurfaces=15, scalars="s")
    vol.contour(isosurfaces=8, scalars="v")
    # cutting / clipping / thresholding
    vol.clip(normal="x")
    vol.slice_orthogonal()
    vol.threshold((20.0, 50.0), scalars="v")
    # geometry / point ops
    sphere.compute_normals(inplace=False)
    grid.warp_by_scalar("s", factor=0.5)
    sphere.glyph(scale=False, orient=False, geom=arrow, tolerance=0.01)
    sphere.smooth(n_iter=40)
    sphere.triangulate().decimate(0.5)
    vol.cell_data["c"] = vol.point_data["v"][: vol.n_cells]; vol.cell_data_to_point_data()

print("pgo training workload complete")
