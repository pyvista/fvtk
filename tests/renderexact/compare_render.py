"""Pixel-exact comparison of two run_render.py output dirs (stock vs cvista).

The assertion is the strictest possible: ``np.array_equal`` on the raw bytes of
the RGBA framebuffer (and the Z buffer where captured). Any single differing
pixel/byte is a failure, not a warning.

Before trusting a diff, it asserts the GL driver matched on both sides
(GL_RENDERER / GL_VERSION) -- otherwise a pixel diff could reflect a different
Mesa, not cvista's code.
"""
from __future__ import annotations

import json
import os

import numpy as np


def load_manifest(d):
    with open(os.path.join(d, "manifest.json")) as fh:
        return json.load(fh)


def compare_case(stock_dir, cvista_dir, name):
    sp = os.path.join(stock_dir, name + ".npz")
    fp = os.path.join(cvista_dir, name + ".npz")
    if not os.path.exists(sp) or not os.path.exists(fp):
        return False, {"reason": "missing npz", "stock": os.path.exists(sp), "cvista": os.path.exists(fp)}
    a = np.load(sp)
    b = np.load(fp)
    names_a, names_b = set(a.files), set(b.files)
    if names_a != names_b:
        return False, {
            "reason": "buffer set mismatch",
            "only_stock": sorted(names_a - names_b),
            "only_cvista": sorted(names_b - names_a),
        }
    per_buf = {}
    ok = True
    for buf in sorted(names_a):
        x, y = a[buf], b[buf]
        equal = bool(x.shape == y.shape and x.dtype == y.dtype and np.array_equal(x, y))
        ndiff = None
        if x.shape == y.shape and x.dtype == y.dtype and not equal:
            diff = x.astype(np.int64) != y.astype(np.int64)
            ndiff = int(np.count_nonzero(np.any(diff.reshape(diff.shape[0], diff.shape[1], -1), axis=-1)))
        per_buf[buf] = {
            "equal": equal,
            "shape_stock": list(x.shape),
            "shape_cvista": list(y.shape),
            "dtype": str(x.dtype),
            "n_diff_pixels": ndiff,
        }
        ok &= equal
    return ok, {"buffers": per_buf}


def compare_all(stock_dir, cvista_dir):
    ms = load_manifest(stock_dir)
    mf = load_manifest(cvista_dir)

    gl_s = ms["provenance"].get("gl", {})
    gl_f = mf["provenance"].get("gl", {})
    prov = {
        "vtk_stock": ms["provenance"]["vtk_version"],
        "vtk_cvista": mf["provenance"]["vtk_version"],
        "numpy_stock": ms["provenance"]["numpy"],
        "numpy_cvista": mf["provenance"]["numpy"],
        "numpy_match": ms["provenance"]["numpy"] == mf["provenance"]["numpy"],
        "gl_renderer_stock": gl_s.get("gl_renderer"),
        "gl_renderer_cvista": gl_f.get("gl_renderer"),
        "gl_version_stock": gl_s.get("gl_version"),
        "gl_version_cvista": gl_f.get("gl_version"),
        "gl_match": (
            gl_s.get("gl_renderer") == gl_f.get("gl_renderer")
            and gl_s.get("gl_version") == gl_f.get("gl_version")
            and gl_s.get("gl_renderer") is not None
        ),
    }

    keys = sorted(set(ms["cases"]) & set(mf["cases"]))
    cases = {}
    for name in keys:
        cs, cf = ms["cases"][name], mf["cases"][name]
        if "error" in cs or "error" in cf:
            cases[name] = {
                "ok": False,
                "detail": {"reason": "scene errored", "stock": cs.get("error"), "cvista": cf.get("error")},
            }
            continue
        ok, detail = compare_case(stock_dir, cvista_dir, name)
        detail["window_class_stock"] = cs.get("window_class")
        detail["window_class_cvista"] = cf.get("window_class")
        cases[name] = {"ok": ok, "detail": detail}
    return {"provenance": prov, "cases": cases, "keys": keys}


if __name__ == "__main__":
    import sys

    res = compare_all(sys.argv[1], sys.argv[2])
    prov = res["provenance"]
    print("== provenance ==")
    print(f"  vtk      stock={prov['vtk_stock']} cvista={prov['vtk_cvista']}")
    print(f"  GL_REND  stock={prov['gl_renderer_stock']!r}")
    print(f"           cvista ={prov['gl_renderer_cvista']!r}")
    print(f"  GL_VER   stock={prov['gl_version_stock']!r}")
    print(f"           cvista ={prov['gl_version_cvista']!r}")
    print(f"  gl_match={prov['gl_match']}  numpy_match={prov['numpy_match']}")
    print("== scenes ==")
    n_ok = 0
    for name in res["keys"]:
        c = res["cases"][name]
        status = "OK   " if c["ok"] else "DIFF "
        bufs = c["detail"].get("buffers", {})
        extra = " ".join(
            f"{b}:diff={info['n_diff_pixels']}" for b, info in bufs.items() if not info["equal"]
        )
        if not c["ok"] and "reason" in c["detail"]:
            extra = c["detail"]["reason"] + " " + str(c["detail"].get("stock", ""))
        print(f"  {status} {name}  {extra}")
        n_ok += c["ok"]
    allok = res["provenance"]["gl_match"] and n_ok == len(res["keys"]) and len(res["keys"]) > 0
    print(f"== {n_ok}/{len(res['keys'])} scenes pixel-exact; gl_match={prov['gl_match']} ==")
    sys.exit(0 if allok else 1)
