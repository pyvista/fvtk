"""Wrapper-behavior parity probe for the abi3 migration.

The numeric bit-exact suite (test_bitexact.py) covers floating-point output.
It does NOT cover the *wrapper runtime* behavior that the abi3 / Py_LIMITED_API
port is most likely to perturb: object identity / isinstance / the type
hierarchy, the numpy zero-copy buffer protocol on vtkDataArray, repr format,
pickling and weakref. This probe captures those facts as a JSON document under
a given backend (stock VTK or fvtk); compare_parity() diffs two such documents.

Run standalone the same way run_ops.py is run:

    python wrapper_parity.py <output_dir>      # writes <output_dir>/parity.json

The harness runs it once under BITEXACT_STOCK_PY and once under BITEXACT_FVTK_PY
and asserts the two parity.json documents are identical (modulo intentionally
backend-specific fields like pointer addresses, which are normalized out).

Importing vtkmodules here resolves to whichever backend the running python
provides (the _fvtk_shim redirect makes `import vtkmodules` load fvtk under the
fvtk venv), exactly like run_ops.py.
"""
from __future__ import annotations

import json
import os
import re
import sys


def _probe():
    """Collect wrapper-behavior facts. Returns a JSON-serializable dict."""
    import numpy as np
    from vtkmodules.vtkCommonCore import vtkObject, vtkObjectBase, vtkDoubleArray, vtkPoints
    from vtkmodules.vtkCommonDataModel import vtkPolyData

    facts = {}

    # --- type hierarchy / identity / flags -------------------------------
    # Heap types (the abi3 FromSpec port target) differ observably from static
    # types in __flags__ (Py_TPFLAGS_HEAPTYPE=1<<9, IMMUTABLETYPE=1<<8) and in
    # whether the type object is mutable. Capture the mro names and the flags
    # bits that distinguish heap vs static so a regression is loud.
    HEAPTYPE = 1 << 9
    IMMUTABLETYPE = 1 << 8
    for cls in (vtkObjectBase, vtkObject, vtkDoubleArray, vtkPoints, vtkPolyData):
        name = cls.__name__
        facts[f"mro::{name}"] = [c.__name__ for c in cls.__mro__]
        facts[f"flag_heaptype::{name}"] = bool(cls.__flags__ & HEAPTYPE)
        facts[f"flag_immutabletype::{name}"] = bool(cls.__flags__ & IMMUTABLETYPE)
        facts[f"qualname::{name}"] = cls.__qualname__
        facts[f"module::{name}"] = getattr(cls, "__module__", None)

    # --- isinstance / issubclass relationships ---------------------------
    arr = vtkDoubleArray()
    facts["isinstance_arr_objectbase"] = isinstance(arr, vtkObjectBase)
    facts["isinstance_arr_object"] = isinstance(arr, vtkObject)
    facts["issubclass_dblarr_objbase"] = issubclass(vtkDoubleArray, vtkObjectBase)
    facts["type_is_vtkDoubleArray"] = (type(arr).__name__ == "vtkDoubleArray")

    # --- repr format (PyVTKObject_Repr) ----------------------------------
    # Format is "<PKG.module.vtkXxx(0xADDR) at 0xADDR>"; normalize the two hex
    # pointers AND the top-level package name. fvtk *intentionally* renames the
    # package from `vtkmodules` to `fvtk` (the defining feature of the fork), so
    # that prefix legitimately differs from stock and is normalized out; only the
    # structural repr shape (which the wrapper runtime controls) is compared.
    pts = vtkPoints()
    _repr = re.sub(r"0x[0-9a-fA-F]+", "0xPTR", repr(pts))
    _repr = re.sub(r"\b(?:fvtk|vtkmodules)\.", "PKG.", _repr)
    facts["repr_format"] = _repr
    facts["str_starts_doublearray"] = vtkObjectBase.__name__ in str(type(arr))

    # --- numpy zero-copy buffer protocol (B5: tp_as_buffer) --------------
    # This is the load-bearing parity check for abi3: np.asarray on a vtk array
    # must be byte-identical AND share memory (zero-copy) with the vtk buffer.
    a = vtkDoubleArray()
    a.SetNumberOfComponents(1)
    a.SetNumberOfTuples(8)
    for i in range(8):
        a.SetValue(i, float(i) * 1.5)
    npview = np.frombuffer(a, dtype=np.float64)
    facts["buffer_len"] = int(npview.shape[0])
    facts["buffer_values"] = [float(x) for x in npview]
    facts["buffer_dtype"] = str(npview.dtype)
    # zero-copy proof: mutate through vtk, observe the change in the numpy view.
    a.SetValue(3, -99.0)
    facts["buffer_zerocopy_shared"] = (float(npview[3]) == -99.0)
    # memoryview protocol facts
    mv = memoryview(a)
    facts["memoryview_itemsize"] = mv.itemsize
    facts["memoryview_format"] = mv.format
    facts["memoryview_readonly"] = mv.readonly
    facts["memoryview_nbytes"] = mv.nbytes

    # --- weakref support -------------------------------------------------
    import weakref
    o = vtkObject()
    try:
        ref = weakref.ref(o)
        facts["weakref_supported"] = (ref() is o)
    except TypeError:
        facts["weakref_supported"] = False

    # --- instance __dict__ (tp_dictoffset / B4) --------------------------
    o2 = vtkObject()
    try:
        o2.some_python_attr = 42  # noqa
        facts["instance_dict_works"] = (o2.some_python_attr == 42)
        facts["instance_dict_in_vars"] = ("some_python_attr" in vars(o2))
    except Exception as e:  # noqa
        facts["instance_dict_works"] = f"ERR:{type(e).__name__}"

    # --- override() machinery (PyVTKClass_override, exercises tp_dict) ----
    facts["has_override_method"] = hasattr(vtkObjectBase, "override")

    return facts


def main():
    if len(sys.argv) < 2:
        print("usage: wrapper_parity.py <output_dir>", file=sys.stderr)
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    facts = _probe()
    with open(os.path.join(out, "parity.json"), "w") as f:
        json.dump(facts, f, indent=2, sort_keys=True)
    print(f"[wrapper_parity] wrote {len(facts)} facts to {out}/parity.json")
    return 0


def compare_parity(stock_dir, fvtk_dir):
    """Return list of (key, stock_value, fvtk_value) mismatches. Empty == parity."""
    with open(os.path.join(stock_dir, "parity.json")) as f:
        s = json.load(f)
    with open(os.path.join(fvtk_dir, "parity.json")) as f:
        v = json.load(f)
    mismatches = []
    for k in sorted(set(s) | set(v)):
        # vtkmodules module name differs intentionally (fvtk renames the package);
        # the migration must not change it, but it is already different from stock
        # by design, so skip the module:: facts from the equality gate.
        if k.startswith("module::"):
            continue
        if s.get(k, "<missing>") != v.get(k, "<missing>"):
            mismatches.append((k, s.get(k, "<missing>"), v.get(k, "<missing>")))
    return mismatches


if __name__ == "__main__":
    sys.exit(main())
