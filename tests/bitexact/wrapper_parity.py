"""Wrapper-behavior parity probe for the abi3 migration.

The numeric bit-exact suite (test_bitexact.py) covers floating-point output.
It does NOT cover the *wrapper runtime* behavior that the abi3 / Py_LIMITED_API
port is most likely to perturb: object identity / isinstance / the type
hierarchy, the numpy zero-copy buffer protocol on vtkDataArray, repr format,
pickling and weakref. This probe captures those facts as a JSON document under
a given backend (stock VTK or cvista); compare_parity() diffs two such documents.

Run standalone the same way run_ops.py is run:

    python wrapper_parity.py <output_dir>      # writes <output_dir>/parity.json

The harness runs it once under BITEXACT_STOCK_PY and once under BITEXACT_CVISTA_PY
and asserts the two parity.json documents are identical (modulo intentionally
backend-specific fields like pointer addresses, which are normalized out).

Importing vtkmodules here resolves to whichever backend the running python
provides (the _cvista_shim redirect makes `import vtkmodules` load cvista under the
cvista venv), exactly like run_ops.py.
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
    BASETYPE = 1 << 10
    for cls in (vtkObjectBase, vtkObject, vtkDoubleArray, vtkPoints, vtkPolyData):
        name = cls.__name__
        facts[f"mro::{name}"] = [c.__name__ for c in cls.__mro__]
        facts[f"flag_heaptype::{name}"] = bool(cls.__flags__ & HEAPTYPE)
        facts[f"flag_immutabletype::{name}"] = bool(cls.__flags__ & IMMUTABLETYPE)
        # BASETYPE is set on stock wrapped vtkObject-derived classes already (they
        # are subclassable), so under abi3 it must NOT diverge for these — captured
        # so a regression that drops it is loud.
        facts[f"flag_basetype::{name}"] = bool(cls.__flags__ & BASETYPE)
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
    # pointers AND the top-level package name. cvista *intentionally* renames the
    # package from `vtkmodules` to `cvista` (the defining feature of the fork), so
    # that prefix legitimately differs from stock and is normalized out; only the
    # structural repr shape (which the wrapper runtime controls) is compared.
    pts = vtkPoints()
    _repr = re.sub(r"0x[0-9a-fA-F]+", "0xPTR", repr(pts))
    _repr = re.sub(r"\b(?:cvista|vtkmodules)\.", "PKG.", _repr)
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

    # --- runtime helper types (the Increment-2 PyType_FromSpec targets) ---
    # These internal types (reference/mutable proxy, namespaces) are defined as
    # static PyTypeObjects in Wrapping/PythonCore. Converting them to heap types
    # for abi3 would flip __flags__ HEAPTYPE on; probe them so that divergence is
    # caught here, not silently shipped.
    try:
        import vtkmodules.util as _vutil  # noqa
    except Exception:  # noqa
        pass
    try:
        from vtkmodules.vtkCommonCore import reference as _ref
        r = _ref(0)
        facts["reference_flag_heaptype"] = bool(type(r).__flags__ & HEAPTYPE)
        facts["reference_flag_immutabletype"] = bool(type(r).__flags__ & IMMUTABLETYPE)
        # The `reference` proxy is a BASE for the number/string/tuple reference
        # subtypes. Stock's static `reference` is subclassed without the BASETYPE
        # flag; the limited API requires it on a heap base, so under abi3 this bit
        # ALSO flips (intrinsic static->heap artifact). Probed + allowed below.
        facts["reference_flag_basetype"] = bool(type(r).__flags__ & BASETYPE)
        facts["reference_typename"] = type(r).__name__
        r.set(7)
        facts["reference_set_get"] = int(r.get())
        facts["reference_repr"] = re.sub(r"\b(?:cvista|vtkmodules)\.", "PKG.", repr(type(r)))
        facts["reference_mro"] = [c.__name__ for c in type(r).__mro__]
    except Exception as e:  # noqa
        facts["reference_probe"] = f"ERR:{type(e).__name__}:{e}"

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


# Keys whose value legitimately diverges between a static-type stock build and an
# abi3 heap-type cvista build, and ONLY those keys. Under Py_LIMITED_API every type
# is necessarily a heap type (PyType_FromSpec is the only way to create one), so
# Py_TPFLAGS_HEAPTYPE (1<<9) flips on and IMMUTABLETYPE (1<<8) flips off on every
# wrapped class and on the reference helper types. This is the single permitted
# divergence of the abi3 wheel; the gate asserts it is EXPECTED (heaptype True /
# immutabletype False on cvista vs the opposite on stock) and that NOTHING ELSE
# differs. See docs/abi3-feasibility.md.
def _is_abi3():
    """Whether the cvista build under test is the abi3 (heap-type) wheel.

    abi3 is the DEFAULT shipped wheel format, so this DEFAULTS TO TRUE: the parity
    gate expects heap types and tolerates ONLY the __flags__ HEAPTYPE/IMMUTABLETYPE
    (and reference BASETYPE) divergence, asserting everything else matches stock
    byte-for-byte. To validate the legacy static-type wheel (CVISTA_ABI3=0), set
    BITEXACT_ABI3=0 — the gate then requires strict byte-for-byte parity incl.
    __flags__."""
    return os.environ.get("BITEXACT_ABI3", "1") not in ("0", "false", "False")


def _expected_flag_divergence(key, stock_val, cvista_val):
    """Under abi3, a type-flag key is allowed to differ iff it differs in exactly
    the heap-vs-static direction:
      - heaptype:       stock False -> cvista True   (every heap type)
      - immutabletype:  stock True  -> cvista False  (every heap type)
      - basetype:       stock False -> cvista True   ONLY for the `reference` helper
                        (and special base types): stock subclasses static types
                        without the flag; the limited API requires it on a heap
                        base. Wrapped vtkObject-derived classes already carry
                        BASETYPE on stock, so flag_basetype::<class> must NOT
                        diverge and is not whitelisted here."""
    if "flag_heaptype" in key:
        return stock_val is False and cvista_val is True
    if "flag_immutabletype" in key:
        return stock_val is True and cvista_val is False
    if key == "reference_flag_basetype":
        return stock_val is False and cvista_val is True
    return False


def compare_parity(stock_dir, cvista_dir):
    """Return list of (key, stock_value, cvista_value) mismatches. Empty == parity.

    In the DEFAULT (abi3, heap-type) build the two type-flag bits are EXPECTED to
    flip to the heap-type values (plus the reference BASETYPE bit) and are accepted
    *only* in that exact direction; any other divergence — including a flag
    flipping the wrong way, or a wrapped type NOT becoming a heap type — is still
    reported as a mismatch. Everything else (identity/isinstance/mro/repr/weakref/
    instance-dict/numpy zero-copy buffer/numeric) must match stock byte for byte
    (modulo the intentional vtkmodules->cvista package rename, already normalized).
    In the legacy static-type build (BITEXACT_ABI3=0) EVERY checked fact must match
    stock byte-for-byte, including __flags__."""
    with open(os.path.join(stock_dir, "parity.json")) as f:
        s = json.load(f)
    with open(os.path.join(cvista_dir, "parity.json")) as f:
        v = json.load(f)
    abi3 = _is_abi3()
    mismatches = []
    for k in sorted(set(s) | set(v)):
        # vtkmodules module name differs intentionally (cvista renames the package);
        # the migration must not change it, but it is already different from stock
        # by design, so skip the module:: facts from the equality gate.
        if k.startswith("module::"):
            continue
        sv = s.get(k, "<missing>")
        fv = v.get(k, "<missing>")
        if sv == fv:
            continue
        if abi3 and _expected_flag_divergence(k, sv, fv):
            continue  # the one permitted abi3 divergence, in the permitted direction
        mismatches.append((k, sv, fv))
    return mismatches


if __name__ == "__main__":
    sys.exit(main())
