# fvtk Python stable-ABI (abi3 / `Py_LIMITED_API`) feasibility

Status: **investigation + diagnostic build lever landed.** No working abi3 wheel
yet — and, as documented below, one is *not* achievable without a substantial
rewrite of the auto-generated Python wrapper runtime. This document is the
evidence-backed blocker inventory and recommended roadmap.

Scope owner: abi3 / wrapper-generation / packaging track. All findings are local
to this worktree; nothing here pushes, opens PRs, or triggers CI.

---

## TL;DR verdict

- **The value is real.** Today the wheel build compiles the Python wrappers
  **five times** — once per CPython minor — because the wrapper `.so`s are
  version-tagged. `pyproject.toml` builds `cp39-* cp310-* cp311-* cp312-*
  cp313-*` (`[tool.cibuildwheel].build`). A single `cp3x-abi3` wheel would
  collapse that matrix to one Python leg (the C++ core is already shared), and
  would auto-support future CPython minors without a rebuild.

- **The cost is large and structural.** The wrapper runtime
  (`Wrapping/PythonCore`) and, more importantly, the **wrapper code generator**
  (`Wrapping/Tools/vtkWrapPython*.c`) are built around *static* `PyTypeObject`
  instances with direct `tp_*` field access and a C-visible
  (`PyObject_HEAD`-derived) instance struct. **Under `Py_LIMITED_API`,
  `PyTypeObject` is an opaque/incomplete type — you cannot statically define one
  or read/write its fields at all.** Every one of the ~1700 wrapped classes
  emits exactly such a static definition.

- **It is *technically possible*** to port to `PyType_FromSpec` heap types
  (proven below — the required shape compiles clean under
  `Py_LIMITED_API=0x030d0000`), but it touches the generator's hottest code path
  and the entire runtime, and it has a **bit-exact risk**: heap types differ
  observably from static types (`type(x).__flags__`, identity of the type
  object, `tp_dictoffset` semantics, GC behavior). For a fork whose contract is
  *byte-for-byte VTK 9.6.2 parity*, that risk is the dominant concern, not the
  C-API mechanics.

**Recommendation: do NOT pursue full abi3 now.** Pursue path (c)/(b) — keep the
per-version wrappers, and treat abi3 as a longer-horizon project gated on a
parity-risk decision. See "Recommended roadmap". The `FVTK_ABI3` lever added
here exists to keep the porting worklist measurable.

---

## What was implemented in this worktree

A build option that compiles the Python wrapper TUs against the limited API, so
the blocker set is reproducible as compiler output rather than prose:

- `fvtk-config/minimal.cmake` — new `option(FVTK_ABI3 ... OFF)` and
  `FVTK_ABI3_VERSION` (default `0x030d0000` = CPython 3.13, the would-be
  `cp313-abi3` floor).
- `CMake/vtkModuleWrapPython.cmake` (~line 692) — when `FVTK_ABI3` is ON, adds
  `Py_LIMITED_API=${FVTK_ABI3_VERSION}` to every generated per-module wrapper
  target (alongside the existing `PYTHON_PACKAGE` define).
- `Wrapping/PythonCore/CMakeLists.txt` — same define on `VTK::WrappingPythonCore`
  (the runtime library) via `vtk_module_definitions`.

It is OFF by default and changes nothing for the normal build. Turning it ON is
a *diagnostic*: the build will fail in the wrapper TUs, and those failures are
the prioritized port worklist.

---

## How the blockers were confirmed

`PyTypeObject` opacity and friends were verified empirically by compiling the
exact constructs the wrappers use against the real CPython 3.12 limited-API
headers (`gcc -DPy_LIMITED_API=0x030d0000 -include Python.h`). Results:

| Probe | Construct | Result under `Py_LIMITED_API` |
|---|---|---|
| static `PyTypeObject Foo = { PyVarObject_HEAD_INIT(...) ... }` | every wrapped class | **FAILS** — `variable 'Foo' has initializer but incomplete type` |
| `t->tp_dict`, `t->tp_base`, `t->tp_dictoffset` | field read/write | **FAILS** — `invalid use of incomplete typedef 'PyTypeObject'` |
| `static PyBufferProcs B = {...}` (`tp_as_buffer`) | `PyVTKObject_AsBuffer` | **FAILS** — `unknown type name 'PyBufferProcs'` |
| `PyHeapTypeObject*` | — | **FAILS** — `unknown type name 'PyHeapTypeObject'` |
| `Py_TPFLAGS_MANAGED_DICT` / `Py_TPFLAGS_MANAGED_WEAKREF` | managed-dict layout | **FAILS** — undeclared (not in limited API) |
| `PyObject_GC_New` / `PyObject_GC_Track` / `PyObject_GC_Del` | instance alloc | **OK** |
| `PyType_FromSpec` / `PyType_GetSlot(t, Py_tp_base)` / `Py_tp_*` slot ids | abi3 port target | **OK** |
| `PyType_FromSpec` + `Py_tp_members` carrying `{"__dictoffset__", T_PYSSIZET, offsetof(PyVTKObject, vtk_dict), READONLY}` and `__weaklistoffset__` | the actual migration shape | **OK, compiles clean** |

The last row is the important positive result: the per-instance dict/weakref
layout that the static types express via `tp_dictoffset =
offsetof(PyVTKObject, vtk_dict)` *can* be reproduced under the limited API via
a `Py_tp_members` slot with synthetic `__dictoffset__`/`__weaklistoffset__`
members. So a port is mechanically feasible; it is the breadth and the
bit-exact parity that make it expensive.

(`PyVTKObject` itself remains a C-visible `PyObject_HEAD`-derived struct, which
is fine under the limited API: `PyObject_HEAD` is part of the stable ABI, and
`offsetof` on a user struct is plain C.)

---

## Blocker inventory (categorized, with file:line evidence)

### B1 — Static `PyTypeObject` instances (the load-bearing blocker)

Generated for **every** wrapped class, special object, enum, and namespace, plus
fixed ones in the runtime. Under limited API these cannot exist.

Generator (emits one per wrapped entity — ~1700 classes × 1, plus every enum /
special type / namespace):
- `Wrapping/Tools/vtkWrapPythonClass.c:454-455` — vtkObject-derived classes
- `Wrapping/Tools/vtkWrapPythonType.c:669-670` — special (value) types
- `Wrapping/Tools/vtkWrapPythonEnum.c:187-188` — enums

Runtime fixed types:
- `Wrapping/PythonCore/PyVTKMethodDescriptor.cxx:149`
- `Wrapping/PythonCore/PyVTKNamespace.cxx:49`
- `Wrapping/PythonCore/PyVTKTemplate.cxx:253`
- `Wrapping/PythonCore/PyVTKReference.cxx:735, 788, 841, 894`

Port target: emit `PyType_Spec` + `PyType_Slot[]` and call `PyType_FromSpec`
(→ heap type). Affects the generator's central type-emission routines
(`vtkWrapPython_GenerateObjectType` and peers).

### B2 — Direct `tp_*` field access on `PyTypeObject*`

59 occurrences in the runtime alone; the generator also emits direct writes.
All become illegal (opaque type). Each read maps to `PyType_GetSlot`; each
write must move into the `PyType_Spec` slot table (writes after `FromSpec` are
mostly disallowed for heap types).

Runtime samples:
- `Wrapping/PythonCore/PyVTKObject.cxx:66,85,101,155,159,166,174,188,191,206,209,222` — `tp_base`, `tp_dict` (already has a `PyType_GetSlot` fallback at :64 for `>=3.10`)
- `Wrapping/PythonCore/PyVTKSpecialObject.cxx:62,64,75,82,118,119,244,250,256,264` — `tp_base`, `tp_str`, `tp_as_sequence`, `tp_dict`
- `Wrapping/PythonCore/PyVTKNamespace.cxx:117,123` — `tp_base->tp_new/tp_init`
- `Wrapping/PythonCore/PyVTKTemplate.cxx:771,779` — `tp_base->tp_new/tp_init`
- `Wrapping/PythonCore/vtkPythonOverload.cxx:249,483-590` — `tp_as_number`, `tp_base` walk
- `Wrapping/PythonCore/vtkPythonUtil.cxx` — `tp_base`, `tp_dict`, `tp_name` (multiple)

Generator-emitted writes:
- `Wrapping/Tools/vtkWrapPythonClass.c:377,384,389` (`pytype->tp_base = ...`), `:418` (`pytype->tp_dict`)
- `Wrapping/Tools/vtkWrapPythonType.c:872,877,895`

### B3 — `VTK_WRAP_PYTHON_SUPPRESS_UNINITIALIZED` (hard-coded static tail layout)

`Wrapping/PythonCore/vtkPythonCompatibility.h:24-34` expands to a
per-`PY_VERSION_HEX` sequence of trailing `PyTypeObject` fields (e.g. 3.13:
`nullptr, 0, nullptr, nullptr, 0, 0,`). This is a *direct dependency on the
non-stable struct layout* — it only makes sense when statically initializing a
`PyTypeObject`. It disappears entirely in the `FromSpec` model.

Used at: `PyVTKMethodDescriptor.cxx:202`, `PyVTKNamespace.cxx:99`,
`PyVTKTemplate.cxx:303`, `PyVTKReference.cxx:785,838,891,944`, and emitted by
the generator at `vtkWrapPythonClass.c:546`, `vtkWrapPythonType.c:764`,
`vtkWrapPythonEnum.c:252`.

### B4 — `offsetof`-based `tp_dictoffset` / `tp_weaklistoffset`

- `Wrapping/Tools/vtkWrapPythonClass.c:497` — `offsetof(PyVTKObject, vtk_weakreflist), // tp_weaklistoffset`
- `Wrapping/Tools/vtkWrapPythonClass.c:529` — `offsetof(PyVTKObject, vtk_dict), // tp_dictoffset`

These offsets are *settable* under limited API only via the `Py_tp_members`
slot with synthetic `__dictoffset__`/`__weaklistoffset__` members (proven above).
Not a hard blocker, but it is a non-obvious translation and a parity-sensitive
one (managed-dict flags `Py_TPFLAGS_MANAGED_DICT` are unavailable, so the
explicit-offset members route is mandatory).

### B5 — `PyBufferProcs` static (buffer protocol / `tp_as_buffer`)

- `Wrapping/PythonCore/PyVTKObject.cxx:638` — `PyVTKObject_AsBuffer` (vtkDataArray → buffer; the numpy zero-copy path)
- `Wrapping/PythonCore/PyVTKReference.cxx:612`

`PyBufferProcs` is not exposed under the limited API. Buffer support moves to
the `Py_bf_getbuffer` / `Py_bf_releasebuffer` slots (available as slot IDs in
recent limited-API levels) inside the `PyType_Spec`. The *bodies*
(`PyBuffer_FillInfo`, `PyBuffer_FillContiguousStrides`) are themselves
limited-API-OK — only the static `PyBufferProcs` struct and the `tp_as_buffer`
wiring need to change.

### B6 — `PyType_Ready` on static types

`PyVTKNamespace.cxx:114`, `PyVTKExtras.cxx:101-103`, `vtkPythonUtil.cxx:248`,
`PyVTKTemplate.cxx:768`, and generator-emitted `PyType_Ready(pytype)`
(`vtkWrapPythonClass.c:437`). `PyType_Ready` *is* in the limited API, but it
operates on an already-built static type; in the `FromSpec` model it is
subsumed by `PyType_FromSpec` and these calls are removed.

### What is already fine (no work needed)

- Instance allocation: `PyObject_GC_New`/`Track`/`Del` (`PyVTKObject.cxx:756,766,365,380`) — limited-API clean.
- `PyVTKObject`/`PyVTKClass` structs themselves (`PyVTKObject.h:50`) — plain `PyObject_HEAD`-derived C structs.
- Method tables (`PyMethodDef`), getset (`PyGetSetDef`), number/sequence method bodies — all limited-API safe; only their attachment via static `tp_*` slots changes.
- `vtkPythonUtil`'s type *map* (string→`PyTypeObject*`) — fine; it stores pointers to heap types just as well.
- One spot already future-proofed: `PyVTKObject.cxx:63-67` uses `PyType_GetSlot(tp, Py_tp_base)` for `>=3.10` with a `tp_base` fallback.

---

## Which path is realistic — (a) vs (b) vs (c)

**(a) Full `Py_LIMITED_API` conversion of the wrapper runtime to
`PyType_FromSpec`/heap types.** Mechanically feasible (proven). But it rewrites
the generator's central type-emission path and the entire `PythonCore` runtime,
and — critically for this fork — **heap types are observably different from
static types**: different `__flags__` (`Py_TPFLAGS_HEAPTYPE` set, `IMMUTABLETYPE`
absent), different identity/immutability, GC-tracked type objects, different
`repr`/pickle edge cases. For a *bit-exact* drop-in, every such difference is a
potential parity failure that the maxULP=0 contract does not cover but downstream
behavioral tests might. High effort, high parity risk.

**(b) Thinner abi3 shim.** Not actually available for *this* runtime: there is no
small shim that makes static `PyTypeObject` work under the limited API — the type
is opaque, full stop. A "shim" here would still mean the full `FromSpec` port of
type creation. So (b) collapses into (a) for the wrapper objects.

**(c) abi3 for a subset.** Possible in principle (e.g. abi3-ify only the
hand-written runtime while leaving generated wrappers per-version), but it buys
**nothing**: the cp-matrix cost is dominated by the ~1700 *generated* wrapper TUs,
not the dozen runtime TUs. A subset that excludes the generated wrappers does not
reduce the build matrix, so it is not worth doing.

**Conclusion:** the only path that delivers the value is (a) — a full generator +
runtime port to heap types — and that is a large, parity-risky effort. There is
no cheap win here.

---

## Recommended roadmap

Given the *bit-exact VTK 9.6.2* mandate, the recommendation is to **defer full
abi3** and instead:

1. **Keep the lever, keep it OFF.** `FVTK_ABI3` is landed as a measurement tool.
   Re-run it after any wrapper-runtime change to watch the blocker count.

2. **Cheaper matrix win first (no ABI change).** The C++ core is already built
   once and shared across the cp matrix (per `pyproject.toml` comments + ccache).
   Confirm the per-cp legs only recompile the wrapper TUs and that ccache is
   effective; if the marginal cost per extra cp leg is already small, the abi3
   payoff shrinks and deferral is clearly correct. (Investigation, ~0.5 day.)

3. **If abi3 is later prioritized, stage path (a):**
   - **Phase 1 — runtime (~1–2 wks).** Port the dozen fixed types in
     `Wrapping/PythonCore` to `PyType_FromSpec`. Replace all B2 field accesses
     with `PyType_GetSlot`, move B5 buffer to `Py_bf_*` slots, delete B3
     (`SUPPRESS_UNINITIALIZED`). Self-contained; testable in isolation.
   - **Phase 2 — generator (~2–4 wks).** Rewrite `vtkWrapPython_GenerateObjectType`
     and the enum/special/namespace emitters to produce `PyType_Spec`/`PyType_Slot`
     tables instead of static `PyTypeObject`s, including the `Py_tp_members`
     `__dictoffset__`/`__weaklistoffset__` translation (B4). Regenerate and build.
   - **Phase 3 — parity gate (open-ended).** This is the real gate, not the code:
     prove the heap-type wrappers don't break the bit-exact / behavioral suites
     (`type().__flags__`, identity, pickling, numpy buffer round-trips,
     `override()`, weakref/dict semantics). If parity holds, switch
     `[tool.cibuildwheel].build` to a single `cp313-*` leg producing
     `cp313-abi3` (drop the SOABI tag in `CMake/vtkModuleWrapPython.cmake` /
     `ci/cibw/fvtk_backend.py`).

   **Rough total: 4–7 engineer-weeks of implementation + an unbounded parity
   tail.** The parity tail, not the C-API port, is what makes this a "later"
   project for a bit-exact fork.

### Effort summary

| Path | Implementation | Parity risk | Matrix payoff | Verdict |
|---|---|---|---|---|
| (a) full FromSpec port | 4–7 wks + parity tail | **High** (heap vs static) | 5 legs → 1 | defer |
| (b) thin shim | n/a (collapses to a) | — | — | not possible |
| (c) subset abi3 | small | low | **none** | not worth it |

---

## Reproducing the diagnostic

```sh
cmake -S . -B build-abi3 -C fvtk-config/minimal.cmake -DFVTK_ABI3=ON
cmake --build build-abi3   # wrapper TUs fail; failures = the B1/B2/B5 worklist
```

(`FVTK_ABI3_VERSION` defaults to `0x030d0000`; lower it only after the runtime is
ported.) The empirical probe results above were obtained directly against the
CPython 3.12 limited-API headers, independent of a full configure.
