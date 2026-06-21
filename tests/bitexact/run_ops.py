"""Standalone driver: run every bit-exactness case via the vtkmodules API and
dump results to an output dir, one .npz per case plus a manifest.json.

Invoked once under the stock-VTK venv and once under the fvtk venv (the
``vtkmodules`` import resolves to whichever backend that venv provides). The
pytest harness and the CI job then compare the two output dirs byte-for-byte.

Usage:  python run_ops.py <output_dir> [op_filter]
  op_filter: optional substring; only run ops whose name contains it.
"""
from __future__ import annotations

import hashlib
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ops  # noqa: E402


def case_key(op_name, dtype_name, size):
    return f"{op_name}__{dtype_name}__{size}"


def array_sha(arrays):
    h = hashlib.sha256()
    for name in sorted(arrays):
        a = np.ascontiguousarray(arrays[name])
        h.update(name.encode())
        h.update(str(a.dtype).encode())
        h.update(str(a.shape).encode())
        h.update(a.tobytes())
    return h.hexdigest()


def main():
    out = sys.argv[1]
    op_filter = sys.argv[2] if len(sys.argv) > 2 else None
    os.makedirs(out, exist_ok=True)

    # Backend / environment provenance — recorded so the comparison can confirm
    # the two runs used the versions we think they did.
    from vtkmodules.vtkCommonCore import vtkVersion

    import vtkmodules

    provenance = {
        "numpy": np.__version__,
        "vtk_version": vtkVersion.GetVTKVersion(),
        "vtkmodules_file": getattr(vtkmodules, "__file__", "?"),
        "inputs_digest_f64": ops.build_inputs_digest(np.float64),
        "inputs_digest_f32": ops.build_inputs_digest(np.float32),
    }

    manifest = {"provenance": provenance, "cases": {}}
    n_ok = n_err = 0
    for op_name, dtype_name, size in ops.iter_cases():
        if op_filter and op_filter not in op_name:
            continue
        key = case_key(op_name, dtype_name, size)
        try:
            arrays = ops.run_case(op_name, dtype_name, size)
            np.savez(os.path.join(out, key + ".npz"), **arrays)
            manifest["cases"][key] = {
                "op": op_name,
                "dtype": dtype_name,
                "size": size,
                "group": ops.OPS[op_name]["group"],
                "order_relaxed": bool(ops.OPS[op_name].get("order_relaxed", False)),
                "points_relaxed": bool(ops.OPS[op_name].get("points_relaxed", False)),
                "corrects_stock": bool(ops.OPS[op_name].get("corrects_stock", False)),
                "point_data_tol": float(ops.OPS[op_name].get("point_data_tol", 0.0)),
                "n_arrays": len(arrays),
                "sha256": array_sha(arrays),
                "arrays": {k: list(np.asarray(v).shape) for k, v in arrays.items()},
            }
            n_ok += 1
        except Exception as e:  # noqa: BLE001
            manifest["cases"][key] = {
                "op": op_name,
                "dtype": dtype_name,
                "size": size,
                "group": ops.OPS[op_name]["group"],
                "error": repr(e),
            }
            n_err += 1
            print(f"ERROR {key}: {e!r}", file=sys.stderr)

    with open(os.path.join(out, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=2, sort_keys=True)

    print(
        f"[run_ops] backend vtkmodules={provenance['vtkmodules_file']} "
        f"vtk={provenance['vtk_version']} numpy={provenance['numpy']}"
    )
    print(f"[run_ops] inputs_digest f64={provenance['inputs_digest_f64'][:16]}")
    print(f"[run_ops] wrote {n_ok} cases, {n_err} errors -> {out}")
    return 1 if n_err else 0


if __name__ == "__main__":
    sys.exit(main())
