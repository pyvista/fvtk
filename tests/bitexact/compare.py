"""Bit-exact comparison of two run_ops.py output dirs (stock vs fvtk).

The assertion is the strictest possible: ``np.array_equal`` on the raw bytes of
every output array (points + every point/cell data array + topology), with the
ULP distance reported as 0 expected. maxULP tolerance is 0 — any nonzero ULP is
a failure, not a warning.
"""
from __future__ import annotations

import json
import os

import numpy as np


def load_manifest(d):
    with open(os.path.join(d, "manifest.json")) as fh:
        return json.load(fh)


def _ulp_distance(x, y):
    """Max ULP distance between two float arrays of identical shape/dtype."""
    if x.shape != y.shape or x.dtype != y.dtype:
        return None
    if x.dtype == np.float64:
        xi = x.view(np.int64).astype(np.int64)
        yi = y.view(np.int64).astype(np.int64)
    elif x.dtype == np.float32:
        xi = x.view(np.int32).astype(np.int64)
        yi = y.view(np.int32).astype(np.int64)
    else:
        return 0  # integer arrays: array_equal covers exactness
    return int(np.abs(xi - yi).max()) if xi.size else 0


# Tags in their canonical vtkPolyData cell-data global-index order. Cell data is
# indexed across all cells as verts, then lines, then polys, then strips; the
# canonical sort must preserve this partition and only reorder WITHIN each group.
_POLY_TAGS = ("verts", "lines", "polys", "strips")


def _cell_records(arrays):
    """Reconstruct a per-cell canonical sort key list + the global cell order.

    Returns (keys, perm) where ``keys`` is the list of canonical per-cell keys in
    GLOBAL cell-data index order, and ``perm`` is an argsort (a permutation of
    range(numCells)) that orders cells canonically while preserving the VTK group
    partition. Cells are keyed by (group_rank, connectivity-tuple) for polydata or
    (cell_type, connectivity-tuple) for unstructured grids -- connectivity is point
    IDs, which are directly comparable because points stay strictly identical.
    Returns None if the array set has no recognizable topology.
    """
    names = set(arrays.files) if hasattr(arrays, "files") else set(arrays)

    def conn_off(tag):
        ck, ok = (f"conn:{tag}", f"off:{tag}") if tag else ("conn", "off")
        if ck in names and ok in names:
            return np.asarray(arrays[ck]).astype(np.int64), np.asarray(arrays[ok]).astype(np.int64)
        return None, None

    keys = []
    # Unstructured grid: single conn/off plus celltypes.
    if "conn" in names and "off" in names:
        conn, off = conn_off(None)
        ctypes = np.asarray(arrays["celltypes"]).astype(np.int64) if "celltypes" in names else None
        for i in range(len(off) - 1):
            cell = tuple(conn[off[i]:off[i + 1]].tolist())
            rank = int(ctypes[i]) if ctypes is not None else 0
            keys.append((rank, len(cell), cell))
    else:
        # PolyData: grouped verts|lines|polys|strips.
        any_topo = False
        for rank, tag in enumerate(_POLY_TAGS):
            conn, off = conn_off(tag)
            if conn is None:
                continue
            any_topo = True
            for i in range(len(off) - 1):
                cell = tuple(conn[off[i]:off[i + 1]].tolist())
                keys.append((rank, len(cell), cell))
        if not any_topo:
            return None
    # Stable argsort: preserve group partition (rank leads the key), order within.
    perm = sorted(range(len(keys)), key=lambda i: keys[i])
    return keys, perm


def _arr_names(x):
    """Array names for either an NpzFile (.files) or a plain dict."""
    return list(x.files) if hasattr(x, "files") else list(x)


def _point_canonicalization(arrays, names):
    """Return (perm, rank, key) canonicalizing POINTS by (coords, point-data).

    perm = argsort of points into canonical order; rank[old_id] = canonical index
    (used to remap connectivity); key = the per-point sort-key matrix. Lets us
    compare meshes whose surface points are emitted in a different order (e.g. the
    vendored extract_surface kernel's hash/thread-dependent compaction order).
    """
    pts = np.ascontiguousarray(arrays["points"]).astype(np.float64)
    cols = [pts.reshape(len(pts), -1)]
    for name in names:
        if name.startswith("pd:"):
            arr = np.asarray(arrays[name])
            cols.append(arr.reshape(len(arr), -1).astype(np.float64))
    key = np.concatenate(cols, axis=1)
    perm = np.lexsort([key[:, j] for j in range(key.shape[1] - 1, -1, -1)])
    rank = np.empty(len(perm), dtype=np.int64)
    rank[perm] = np.arange(len(perm), dtype=np.int64)
    return perm, rank, key


def _remap_conn(arrays, rank):
    """A dict copy of arrays with every conn[:]/conn:<tag> remapped through rank."""
    d = {n: arrays[n] for n in (arrays.files if hasattr(arrays, "files") else arrays)}
    for n in list(d):
        if n == "conn" or n.startswith("conn:"):
            d[n] = rank[np.asarray(d[n]).astype(np.int64)]
    return d


def _compare_cells(a, b, names, per, ok):
    """Compare cells as a multiset keyed by (group/celltype, connectivity) carrying
    their cell-data. a/b may be NpzFile or plain dict (post point-remap)."""
    ra, rb = _cell_records(a), _cell_records(b)
    if ra is None or rb is None:
        per["__cells__"] = {"equal": False, "reason": "no topology to canonicalize"}
        return False
    ka, pa = ra
    kb, pb = rb
    keys_eq = bool(len(ka) == len(kb) and [ka[i] for i in pa] == [kb[i] for i in pb])
    per["__cells__"] = {"equal": keys_eq, "mode": "order-relaxed", "ncells": len(ka)}
    ok &= keys_eq
    for name in names:
        if not name.startswith("cd:"):
            continue
        x, y = a[name], b[name]
        if x.shape[0] != len(pa) or y.shape[0] != len(pb) or x.shape[1:] != y.shape[1:]:
            per[name] = {"equal": False, "mode": "order-relaxed", "reason": "shape"}
            ok = False
            continue
        xs, ys = x[pa], y[pb]
        if xs.dtype.kind in "iu" and ys.dtype.kind in "iu":
            eq = bool(np.array_equal(xs.astype(np.int64), ys.astype(np.int64)))
        else:
            eq = bool(xs.dtype == ys.dtype and np.array_equal(xs, ys))
        per[name] = {"equal": eq, "mode": "order-relaxed", "dtype": str(x.dtype)}
        ok &= eq
    return ok


def _compare_order_relaxed(a, b, relax_points=False):
    """Order-invariant mesh equality. cells are always compared as a multiset
    carrying their cell-data. Points + point-data are STRICT unless relax_points,
    in which case points are canonicalized by (coords, point-data) and connectivity
    is remapped accordingly (for filters whose surface points are reordered too)."""
    per = {}
    ok = True
    names = sorted(set(_arr_names(a)) & set(_arr_names(b)))
    if not relax_points:
        # points + point-data STRICT (cutter/contour keep point order identical).
        for name in names:
            if name == "points" or name.startswith("pd:"):
                x, y = a[name], b[name]
                eq = bool(x.shape == y.shape and x.dtype == y.dtype and np.array_equal(x, y))
                per[name] = {"equal": eq, "mode": "strict", "dtype": str(x.dtype)}
                ok &= eq
        ok = _compare_cells(a, b, names, per, ok)
        return bool(ok), per
    # relax_points: canonicalize points by (coords, point-data) on both sides.
    pa_perm, ranka, ka = _point_canonicalization(a, names)
    pb_perm, rankb, kb = _point_canonicalization(b, names)
    pts_eq = bool(ka.shape == kb.shape and np.array_equal(ka[pa_perm], kb[pb_perm]))
    per["__points__"] = {"equal": pts_eq, "mode": "points-relaxed", "npoints": int(ka.shape[0])}
    ok &= pts_eq
    # compare cells on point-remapped connectivity.
    ok = _compare_cells(_remap_conn(a, ranka), _remap_conn(b, rankb), names, per, ok)
    return bool(ok), per


def compare_case(stock_dir, fvtk_dir, key, order_relaxed=False, points_relaxed=False):
    """Return (ok: bool, detail: dict) for a single case key."""
    sp = os.path.join(stock_dir, key + ".npz")
    fp = os.path.join(fvtk_dir, key + ".npz")
    if not os.path.exists(sp) or not os.path.exists(fp):
        return False, {"reason": "missing npz", "stock": os.path.exists(sp), "fvtk": os.path.exists(fp)}
    a = np.load(sp)
    b = np.load(fp)
    names_a, names_b = set(a.files), set(b.files)
    if names_a != names_b:
        return False, {
            "reason": "array set mismatch",
            "only_stock": sorted(names_a - names_b),
            "only_fvtk": sorted(names_b - names_a),
        }
    if order_relaxed or points_relaxed:
        # Order-relaxed mesh equality: same multiset of cells carrying their
        # cell-data, cell ORDER negotiable. With points_relaxed, surface POINT
        # order is negotiable too (canonicalized by coords+point-data, connectivity
        # remapped) -- for kernels that emit surface points in their own order.
        ok, per = _compare_order_relaxed(a, b, relax_points=points_relaxed)
        return ok, {"arrays": per, "order_relaxed": True, "points_relaxed": points_relaxed}
    per_array = {}
    ok = True
    for name in sorted(names_a):
        x, y = a[name], b[name]
        # Width-relaxed comparison for INTEGER arrays (fvtk-wide int32-default
        # rule): integer VALUES are sacred, but the storage CONTAINER WIDTH is
        # negotiable -- fvtk defaults topology/index/offset arrays to int32 while
        # stock VTK uses int64. So for integer-kind arrays compare values
        # normalized to int64 and ignore the dtype tag. FLOAT (and other) arrays
        # stay strict: identical dtype + exact bytes (maxULP=0).
        if x.dtype.kind in "iu" and y.dtype.kind in "iu":
            equal = bool(
                x.shape == y.shape
                and np.array_equal(x.astype(np.int64), y.astype(np.int64))
            )
        else:
            equal = bool(
                x.shape == y.shape
                and x.dtype == y.dtype
                and np.array_equal(x, y)
            )
        ulp = None
        if x.dtype.kind == "f" and x.shape == y.shape and x.dtype == y.dtype:
            ulp = _ulp_distance(x, y)
        per_array[name] = {
            "equal": equal,
            "shape_stock": list(x.shape),
            "shape_fvtk": list(y.shape),
            "dtype": str(x.dtype),
            "ulp": ulp,
        }
        ok &= equal
    return ok, {"arrays": per_array}


def compare_all(stock_dir, fvtk_dir):
    """Compare every case present in BOTH manifests. Returns a results dict."""
    ms = load_manifest(stock_dir)
    mf = load_manifest(fvtk_dir)

    # Provenance sanity: numpy versions must match (bit-identical inputs).
    prov = {
        "numpy_stock": ms["provenance"]["numpy"],
        "numpy_fvtk": mf["provenance"]["numpy"],
        "numpy_match": ms["provenance"]["numpy"] == mf["provenance"]["numpy"],
        "vtk_stock": ms["provenance"]["vtk_version"],
        "vtk_fvtk": mf["provenance"]["vtk_version"],
        "inputs_digest_f64_stock": ms["provenance"]["inputs_digest_f64"],
        "inputs_digest_f64_fvtk": mf["provenance"]["inputs_digest_f64"],
        "inputs_digest_match": (
            ms["provenance"]["inputs_digest_f64"]
            == mf["provenance"]["inputs_digest_f64"]
            and ms["provenance"]["inputs_digest_f32"]
            == mf["provenance"]["inputs_digest_f32"]
        ),
    }

    keys = sorted(set(ms["cases"]) & set(mf["cases"]))
    cases = {}
    for key in keys:
        # Skip cases that errored on either side — report them separately.
        cs, cf = ms["cases"][key], mf["cases"][key]
        if "error" in cs or "error" in cf:
            cases[key] = {
                "ok": False,
                "detail": {"reason": "op errored", "stock": cs.get("error"), "fvtk": cf.get("error")},
                "group": cs.get("group"),
            }
            continue
        # A case is order/points-relaxed if EITHER manifest marks it (both agree).
        order_relaxed = bool(cs.get("order_relaxed") or cf.get("order_relaxed"))
        points_relaxed = bool(cs.get("points_relaxed") or cf.get("points_relaxed"))
        ok, detail = compare_case(
            stock_dir, fvtk_dir, key, order_relaxed=order_relaxed, points_relaxed=points_relaxed)
        cases[key] = {"ok": ok, "detail": detail, "group": cs.get("group"),
                      "order_relaxed": order_relaxed or points_relaxed}
    return {"provenance": prov, "cases": cases, "keys": keys}
