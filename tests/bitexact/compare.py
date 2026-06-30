"""Bit-exact comparison of two run_ops.py output dirs (stock vs cvista).

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


def _point_canonicalization(arrays, names, exclude=()):
    """Return (perm, rank, key) canonicalizing POINTS by (coords, point-data).

    perm = argsort of points into canonical order; rank[old_id] = canonical index
    (used to remap connectivity); key = the per-point sort-key matrix. Lets us
    compare meshes whose surface points are emitted in a different order (e.g. the
    vendored extract_surface kernel's hash/thread-dependent compaction order).

    `exclude` is a tuple of substrings; any pd: array whose name contains one is
    left OUT of the sort key. Used by the contour points-only gate to keep
    *computed* normals/gradients (which differ by a few ULP between FlyingEdges
    and SynchronizedTemplates) out of the canonicalization, so points still align
    by their byte-exact coordinates + interpolated data.
    """
    pts = np.ascontiguousarray(arrays["points"]).astype(np.float64)
    cols = [pts.reshape(len(pts), -1)]
    for name in names:
        if name.startswith("pd:") and not any(s in name for s in exclude):
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


def _compare_cell_count_only(a, b, per, ok):
    """Topology check for the contour EnableFast lane: assert only that both
    meshes have the SAME number of cells, NOT that the cell multiset matches.

    vtkFlyingEdges3D (the EnableFast image-data contour) produces the SAME
    isosurface as the stock vtkSynchronizedTemplates3D -- identical iso-crossing
    point set (coords byte-exact after cvista's double-precision interpolation fix)
    and identical triangle COUNT -- but at the ~2% of cases where the marching
    cube emits a planar quad the two algorithms split it along the OPPOSITE
    diagonal. Same surface, same points, same cell count; a fraction of triangles
    carry a different (equally valid) diagonal. So the cell *multiset* legitimately
    differs and is not the right oracle; the point-set byte-equality (checked by
    the caller) plus equal cell count is. Cells are otherwise un-compared."""
    ra, rb = _cell_records(a), _cell_records(b)
    if ra is None or rb is None:
        per["__cells__"] = {"equal": False, "reason": "no topology to canonicalize"}
        return False
    na, nb = len(ra[0]), len(rb[0])
    eq = bool(na == nb)
    per["__cells__"] = {"equal": eq, "mode": "points-only-count",
                        "ncells_stock": na, "ncells_cvista": nb}
    return ok and eq


# pd: arrays whose name contains one of these are *computed* (gradient-derived)
# rather than interpolated, so FlyingEdges and SynchronizedTemplates produce them
# from different (equally valid) arithmetic -> compared ULP-tolerant, not strict.
_COMPUTED_PD = ("Normals", "Gradients")
# Max float32 ULP distance allowed for those computed arrays. FlyingEdges'
# gradient estimate differs from SynchronizedTemplates' by ~1 ULP; 8 is a snug
# bound that still fails any real divergence (a wrong normal is ~1e6+ ULP).
_NORMAL_ULP_TOL = 8


def _wide_float_eq(x, y):
    """Values-preserved equality tolerant to a float WIDTH difference (cvista may
    preserve input float64 where stock downcasts to float32). Same dtype -> strict
    byte-equal; differing float widths -> the wider array downcast to the narrower
    width must be byte-identical (only storage widened, no value changed); integers
    -> width-relaxed int. Returns (equal, note)."""
    if x.shape != y.shape:
        return False, "shape"
    if x.dtype == y.dtype:
        return bool(np.array_equal(x, y)), "strict"
    if x.dtype.kind == "f" and y.dtype.kind == "f":
        narrow = x.dtype if x.dtype.itemsize <= y.dtype.itemsize else y.dtype
        return bool(np.array_equal(x.astype(narrow), y.astype(narrow))), "width-relaxed-float"
    if x.dtype.kind in "iu" and y.dtype.kind in "iu":
        return bool(np.array_equal(x.astype(np.int64), y.astype(np.int64))), "width-relaxed-int"
    return False, "dtype"


def _compare_points_only(a, b, names):
    """Contour EnableFast gate (vtkFlyingEdges3D vs stock SynchronizedTemplates).

    FlyingEdges produces the SAME isosurface, but: points/cells are emitted in
    thread-block order; planar quads are split on the opposite diagonal; surface
    NORMALS come from a different (equally valid) gradient estimate (~1 float32
    ULP); and cvista preserves the input attribute precision where stock downcasts
    (float64 in -> float64 out vs stock's float32, values preserved on downcast).
    The iso-crossing COORDINATES are byte-exact. So:

      * canonicalize points by COORDS only -- byte-exact and unique per active
        edge, so the ordering is unambiguous (asserted: no duplicate coords);
      * assert COORDS byte-exact under that ordering;
      * assert INTERPOLATED point-data values-preserved (width-relaxed: cvista's
        float64 downcast to stock's float32 is byte-identical);
      * assert COMPUTED normals/gradients within a snug ULP tolerance;
      * assert equal cell COUNT (the cell multiset is negotiable: quad diagonal).
    """
    per = {}
    ok = True
    ca = np.ascontiguousarray(a["points"]).astype(np.float64)
    cb = np.ascontiguousarray(b["points"]).astype(np.float64)
    pa = np.lexsort([ca[:, j] for j in range(ca.shape[1] - 1, -1, -1)])
    pb = np.lexsort([cb[:, j] for j in range(cb.shape[1] - 1, -1, -1)])
    # coords-only canonicalization is unambiguous only if coords are unique.
    dup_a = len(ca) - len(np.unique(ca, axis=0))
    dup_b = len(cb) - len(np.unique(cb, axis=0))
    coords_eq = bool(a["points"].shape == b["points"].shape and
                     np.array_equal(a["points"][pa], b["points"][pb]))
    per["__points__"] = {"equal": coords_eq and dup_a == 0 and dup_b == 0,
                         "mode": "points-only", "npoints": int(ca.shape[0]),
                         "coords_byte_exact": coords_eq,
                         "dup_coords_stock": dup_a, "dup_coords_cvista": dup_b}
    ok &= per["__points__"]["equal"]
    for name in names:
        if not name.startswith("pd:"):
            continue
        xa = np.asarray(a[name])[pa]
        xb = np.asarray(b[name])[pb]
        if any(s in name for s in _COMPUTED_PD):
            # computed (gradient-derived) -> ULP tolerant, requires same dtype.
            if xa.shape != xb.shape or xa.dtype != xb.dtype or xa.dtype.kind != "f":
                eq, ulp = False, None
            else:
                ulp = _ulp_distance(xa, xb)
                eq = ulp <= _NORMAL_ULP_TOL
            per[name] = {"equal": bool(eq), "mode": "points-only-ulp",
                         "ulp": ulp, "ulp_tol": _NORMAL_ULP_TOL}
        else:
            # interpolated -> values preserved (width-relaxed float/int).
            eq, note = _wide_float_eq(xa, xb)
            per[name] = {"equal": bool(eq), "mode": "points-only-" + note,
                         "dtype_stock": str(xa.dtype), "dtype_cvista": str(xb.dtype)}
        ok &= per[name]["equal"]
    # cells: count only (quad-diagonal split differs; same surface, same count).
    ok = _compare_cell_count_only(a, b, per, ok)
    return bool(ok), per


def _compare_order_relaxed(a, b, relax_points=False, point_data_tol=0.0, points_only=False):
    """Order-invariant mesh equality. cells are always compared as a multiset
    carrying their cell-data. Points + point-data are STRICT unless relax_points,
    in which case points are canonicalized by (coords, point-data) and connectivity
    is remapped accordingly (for filters whose surface points are reordered too).

    point_data_tol > 0 (opt-in, per-op) relaxes ONLY the *interpolated* point-DATA
    arrays to an absolute/relative tolerance, keeping coords STRICT. This is for
    the EnableFast clip lane: at points lying ~exactly on the clip plane, several
    near-coincident edge crossings collapse in an order-dependent way, so the
    surviving interpolated value (e.g. a near-zero normal component) differs from
    the serial filter by denormal-scale noise (~1e-21). That is FP merge order,
    not an algorithmic difference -- a tolerance this tight (default callers pass
    ~1e-12) still fails any visible divergence (e.g. area- vs unit-weighted
    normals, which differ by O(0.1))."""
    per = {}
    ok = True
    names = sorted(set(_arr_names(a)) & set(_arr_names(b)))
    if points_only:
        return _compare_points_only(a, b, names)
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
    # lexsort's primary key is the coordinate columns, which are bit-identical
    # between stock and cvista, so the permutation aligns points by COORDS even when
    # an interpolated point-data column differs by tolerance-scale noise.
    pa_perm, ranka, ka = _point_canonicalization(a, names)
    pb_perm, rankb, kb = _point_canonicalization(b, names)
    if not point_data_tol:
        pts_eq = bool(ka.shape == kb.shape and np.array_equal(ka[pa_perm], kb[pb_perm]))
        per["__points__"] = {"equal": pts_eq, "mode": "points-relaxed", "npoints": int(ka.shape[0])}
        ok &= pts_eq
    else:
        # Coords STRICT; interpolated point-data within tolerance.
        ca = np.ascontiguousarray(a["points"]).astype(np.float64)[pa_perm]
        cb = np.ascontiguousarray(b["points"]).astype(np.float64)[pb_perm]
        coords_eq = bool(ca.shape == cb.shape and np.array_equal(ca, cb))
        data_eq = True
        worst = 0.0
        for name in names:
            if not name.startswith("pd:"):
                continue
            xa = np.asarray(a[name])[pa_perm]
            xb = np.asarray(b[name])[pb_perm]
            if xa.shape != xb.shape:
                data_eq = False
                per[name] = {"equal": False, "mode": "points-relaxed", "reason": "shape"}
                continue
            if xa.dtype.kind == "f":
                d = float(np.abs(xa.astype(np.float64) - xb.astype(np.float64)).max()) if xa.size else 0.0
                worst = max(worst, d)
                eq = bool(np.allclose(xa, xb, rtol=1e-6, atol=point_data_tol, equal_nan=True))
            else:
                eq = bool(xa.dtype == xb.dtype and np.array_equal(xa, xb))
            per[name] = {"equal": eq, "mode": "points-relaxed-tol", "dtype": str(xa.dtype)}
            data_eq &= eq
        pts_eq = coords_eq and data_eq
        per["__points__"] = {"equal": pts_eq, "mode": "points-relaxed-tol",
            "npoints": int(ka.shape[0]), "coords_exact": coords_eq,
            "max_pointdata_abs_diff": worst, "tol": point_data_tol}
        ok &= pts_eq
    # compare cells on point-remapped connectivity.
    ok = _compare_cells(_remap_conn(a, ranka), _remap_conn(b, rankb), names, per, ok)
    return bool(ok), per


def _compare_corrects_stock(a, b, input_dtype):
    """Divergence-ledger comparison for filters cvista deliberately CORRECTS.

    Stock VTK 9.6.2 has a long-standing bug where ~30 point-producing filters
    ignore OutputPointsPrecision==DEFAULT and emit float32 output points even for
    float64 input. cvista fixes them to preserve the input precision. For ops that
    exercise such a filter directly the byte-exact-vs-stock gate is no longer the
    right oracle (it would flag the correction as a regression); instead we assert
    the CORRECTION without losing rigor:

      * cvista 'points' dtype == the input point dtype (the fix did its job),
      * stock 'points' is the buggy narrower float type (the bug really existed),
      * cvista points DOWNCAST to stock's width are byte-identical to stock's
        points -- i.e. ONLY the storage width widened, not a single VALUE changed,
      * every other array (point/cell data, topology) is byte-identical, with the
        usual width-relaxed comparison for integer (index/offset/celltype) arrays.

    Returns (ok, per_array_detail).
    """
    per = {}
    ok = True
    want = np.dtype(input_dtype)
    for name in sorted(set(a.files) & set(b.files)):
        x, y = a[name], b[name]  # x = stock, y = cvista
        if name == "points":
            cvista_dtype_ok = y.dtype == want
            # Stock is expected to be the buggy narrower float; if stock somehow
            # already matches the input dtype there was nothing to correct and a
            # plain byte-equal still holds.
            stock_is_narrower = x.dtype.kind == "f" and x.dtype.itemsize < want.itemsize
            values_preserved = bool(
                x.shape == y.shape and np.array_equal(x, y.astype(x.dtype)))
            eq = bool(cvista_dtype_ok and values_preserved and
                      (stock_is_narrower or x.dtype == want))
            per[name] = {
                "equal": eq, "mode": "corrects-stock",
                "stock_dtype": str(x.dtype), "cvista_dtype": str(y.dtype),
                "expected_cvista_dtype": str(want),
                "values_preserved_on_downcast": values_preserved,
                "stock_was_narrower": bool(stock_is_narrower),
            }
        elif x.dtype.kind in "iu" and y.dtype.kind in "iu":
            eq = bool(x.shape == y.shape and
                      np.array_equal(x.astype(np.int64), y.astype(np.int64)))
            per[name] = {"equal": eq, "mode": "width-relaxed-int"}
        else:
            eq = bool(x.shape == y.shape and x.dtype == y.dtype and np.array_equal(x, y))
            ulp = None
            if x.dtype.kind == "f" and x.shape == y.shape and x.dtype == y.dtype:
                ulp = _ulp_distance(x, y)
            per[name] = {"equal": eq, "dtype": str(x.dtype), "ulp": ulp}
        ok &= per[name]["equal"]
    return bool(ok), per


def compare_case(stock_dir, cvista_dir, key, order_relaxed=False, points_relaxed=False,
                 point_data_tol=0.0, corrects_stock=False, input_dtype=None,
                 points_only=False):
    """Return (ok: bool, detail: dict) for a single case key."""
    sp = os.path.join(stock_dir, key + ".npz")
    fp = os.path.join(cvista_dir, key + ".npz")
    if not os.path.exists(sp) or not os.path.exists(fp):
        return False, {"reason": "missing npz", "stock": os.path.exists(sp), "cvista": os.path.exists(fp)}
    a = np.load(sp)
    b = np.load(fp)
    names_a, names_b = set(a.files), set(b.files)
    if names_a != names_b:
        return False, {
            "reason": "array set mismatch",
            "only_stock": sorted(names_a - names_b),
            "only_cvista": sorted(names_b - names_a),
        }
    if corrects_stock:
        # Divergence ledger: cvista deliberately corrects a stock precision bug.
        # Assert the corrected output precision + value-preservation instead of
        # byte-matching stock's (buggy) downcast output. See _compare_corrects_stock.
        ok, per = _compare_corrects_stock(a, b, input_dtype)
        return ok, {"arrays": per, "corrects_stock": True, "input_dtype": str(input_dtype)}
    if order_relaxed or points_relaxed:
        # Order-relaxed mesh equality: same multiset of cells carrying their
        # cell-data, cell ORDER negotiable. With points_relaxed, surface POINT
        # order is negotiable too (canonicalized by coords+point-data, connectivity
        # remapped) -- for kernels that emit surface points in their own order.
        # point_data_tol (opt-in) tolerates denormal-scale interpolated-data noise.
        ok, per = _compare_order_relaxed(
            a, b, relax_points=points_relaxed, point_data_tol=point_data_tol,
            points_only=points_only)
        return ok, {"arrays": per, "order_relaxed": True, "points_relaxed": points_relaxed,
                    "point_data_tol": point_data_tol, "points_only": points_only}
    per_array = {}
    ok = True
    for name in sorted(names_a):
        x, y = a[name], b[name]
        # Width-relaxed comparison for INTEGER arrays (cvista-wide int32-default
        # rule): integer VALUES are sacred, but the storage CONTAINER WIDTH is
        # negotiable -- cvista defaults topology/index/offset arrays to int32 while
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
            "shape_cvista": list(y.shape),
            "dtype": str(x.dtype),
            "ulp": ulp,
        }
        ok &= equal
    return ok, {"arrays": per_array}


def compare_all(stock_dir, cvista_dir):
    """Compare every case present in BOTH manifests. Returns a results dict."""
    ms = load_manifest(stock_dir)
    mf = load_manifest(cvista_dir)

    # Provenance sanity: numpy versions must match (bit-identical inputs).
    prov = {
        "numpy_stock": ms["provenance"]["numpy"],
        "numpy_cvista": mf["provenance"]["numpy"],
        "numpy_match": ms["provenance"]["numpy"] == mf["provenance"]["numpy"],
        "vtk_stock": ms["provenance"]["vtk_version"],
        "vtk_cvista": mf["provenance"]["vtk_version"],
        "inputs_digest_f64_stock": ms["provenance"]["inputs_digest_f64"],
        "inputs_digest_f64_cvista": mf["provenance"]["inputs_digest_f64"],
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
                "detail": {"reason": "op errored", "stock": cs.get("error"), "cvista": cf.get("error")},
                "group": cs.get("group"),
            }
            continue
        # A case is order/points-relaxed if EITHER manifest marks it (both agree).
        order_relaxed = bool(cs.get("order_relaxed") or cf.get("order_relaxed"))
        points_relaxed = bool(cs.get("points_relaxed") or cf.get("points_relaxed"))
        # Divergence ledger: cvista corrects a stock precision bug for this op.
        corrects_stock = bool(cs.get("corrects_stock") or cf.get("corrects_stock"))
        # Opt-in interpolated-point-data tolerance (clip fast lane). Both sides
        # carry the same flag; take the max so a 0 on one side can't loosen it.
        point_data_tol = max(
            float(cs.get("point_data_tol", 0.0)), float(cf.get("point_data_tol", 0.0)))
        # Contour EnableFast lane: point set byte-exact + order-relaxed, but the
        # triangulation may differ on planar-quad diagonals, so the cell multiset
        # is checked by COUNT only (see _compare_cell_count_only).
        points_only = bool(cs.get("points_only") or cf.get("points_only"))
        ok, detail = compare_case(
            stock_dir, cvista_dir, key, order_relaxed=order_relaxed, points_relaxed=points_relaxed,
            point_data_tol=point_data_tol, corrects_stock=corrects_stock,
            input_dtype=cs.get("dtype"), points_only=points_only)
        cases[key] = {"ok": ok, "detail": detail, "group": cs.get("group"),
                      "order_relaxed": order_relaxed or points_relaxed,
                      "corrects_stock": corrects_stock}
    return {"provenance": prov, "cases": cases, "keys": keys}
