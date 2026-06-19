"""Compare hardware-selection (picking) output between two run_select.py dirs.

Asserts that the set of selected ORIGINAL point ids is identical between stock
VTK and fvtk for every scene. The ids are integer values (width-relaxed): fvtk's
source vtkOriginalPointIds array is int32, stock's is int64, but the selected
VALUES must match exactly. A mismatch means fvtk's mapper failed to remap picked
ids through the int32 passthrough array (the regression this gate guards).

Also enforces:
  * the selection is non-empty (an empty selection would trivially "match"),
  * neither backend errored,
  * (informational) the fvtk source array really is int32 -- proving the
    discriminating condition holds; a warning, not a failure, so the gate keeps
    working if the storage policy ever changes.

Usage:  python compare_select.py <stock_dir> <fvtk_dir>   (exit 1 on any diff)
"""
from __future__ import annotations

import json
import os
import sys

import numpy as np


def _load(d):
    with open(os.path.join(d, "manifest.json")) as fh:
        return json.load(fh)


def main():
    stock_dir, fvtk_dir = sys.argv[1], sys.argv[2]
    sm = _load(stock_dir)
    fm = _load(fvtk_dir)

    scenes = sorted(set(sm["cases"]) | set(fm["cases"]))
    n_fail = 0
    for name in scenes:
        sc = sm["cases"].get(name, {})
        fc = fm["cases"].get(name, {})
        if "error" in sc or "error" in fc:
            print(f"FAIL {name}: backend error stock={sc.get('error')} fvtk={fc.get('error')}")
            n_fail += 1
            continue
        s_ids = np.load(os.path.join(stock_dir, name + ".npz"))["selected_point_ids"]
        f_ids = np.load(os.path.join(fvtk_dir, name + ".npz"))["selected_point_ids"]
        if s_ids.size == 0:
            print(f"FAIL {name}: stock selection is EMPTY (non-discriminating)")
            n_fail += 1
            continue
        if s_ids.shape != f_ids.shape or not np.array_equal(
            s_ids.astype(np.int64), f_ids.astype(np.int64)
        ):
            only_s = np.setdiff1d(s_ids, f_ids)
            only_f = np.setdiff1d(f_ids, s_ids)
            print(
                f"FAIL {name}: selected original point ids differ "
                f"(stock n={s_ids.size}, fvtk n={f_ids.size}; "
                f"only-stock={only_s[:8].tolist()}, only-fvtk={only_f[:8].tolist()})"
            )
            n_fail += 1
            continue
        # Informational: confirm the discriminating int32 storage is in effect.
        fdt = fc.get("orig_pointids_dtype")
        note = "" if fdt == "int32" else f"  [warn: fvtk dtype={fdt}, expected int32]"
        print(f"OK   {name}: {s_ids.size} selected ids match "
              f"(stock dtype={sc.get('orig_pointids_dtype')}, fvtk dtype={fdt}){note}")

    print(f"[compare_select] {len(scenes) - n_fail}/{len(scenes)} scenes matched")
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
