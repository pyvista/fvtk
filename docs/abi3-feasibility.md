# fvtk Python stable-ABI (abi3 / `Py_LIMITED_API`) feasibility

Status: **incremental ladder in progress.** The original feasibility pass
(below) correctly concluded that a *complete* abi3 wheel needs a generator +
runtime port with an unbounded parity tail, and recommended deferral. The
active decision is **not** to defer wholesale but to land that port as a ladder
of *independently valuable, bit-exact* increments, so each commit improves fvtk
(CI time / API hygiene / perf) on its own and the abi3 endpoint is approached
without a big-bang. See the **Increment status log** immediately below; the
original blocker inventory and roadmap follow it unchanged for reference.

---

## Increment status log (newest first)

### Increment 3 (generator port START + the matrix-payoff MEASUREMENT) — spec-emission LANDED; runtime-side seam pinned; numbers measured

Two halves: (a) port the wrapper generator's central type-emission routine to
emit a `PyType_Spec` under abi3, and (b) **measure** the wrapper-TU compile cost
and project the cp-matrix savings the abi3 wheel buys. Both done; the runtime
glue (`Py%s_ClassNew`/`PyVTKClass_Add`) that consumes the spec is the documented
next pickup (it reopens the `tp_dict`-population seam from Increment 2).

**(a) Generator spec-emission — LANDED.** `vtkWrapPython_GenerateObjectType`
(`Wrapping/Tools/vtkWrapPythonClass.c`) now wraps its static-`PyTypeObject`
emission in `#if defined(Py_LIMITED_API) … #else … #endif` and, in the
limited-API branch, emits a new `vtkWrapPython_GenerateObjectSpec`: a
`PyType_Slot[]` + `PyType_Spec` mirroring every static field —
`tp_dealloc/repr/str/getattro/setattro/doc/traverse/getset/init/new/free`,
the **buffer protocol via `Py_bf_getbuffer`/`Py_bf_releasebuffer`** slots (pointing
at the now-externally-linked `PyVTKObject_AsBuffer_GetBuffer/ReleaseBuffer`), and
the per-instance dict/weakref layout via a **`Py_tp_members` synthetic
`__dictoffset__`/`__weaklistoffset__`** pair (`Py_T_PYSSIZET`/`Py_READONLY`), plus
the `vtkAlgorithm`/`vtkCollection`/`vtkCollectionIterator` special slots and the
`BASETYPE|HAVE_GC` flags. Verified: a default rebuild (FVTK_ABI3 OFF) regenerates
+ compiles vtkCommonCore wrappers clean and byte-identical (the `#else` static
path is unchanged; the added lines are preprocessor-only). The generated
`vtkObjectPython.cxx` now carries the `PyvtkObject_Spec`/`PyvtkObject_Slots` block
under `#if defined(Py_LIMITED_API)`.

  *Not yet wired (documented next pickup):* `Py%s_ClassNew()` still passes
  `&Py%s_Type` to `PyVTKClass_Add`; the abi3 path must instead `PyType_FromSpec`
  the spec, set `tp_base` at runtime (cross-module base resolved via
  `vtkPythonUtil::FindBaseTypeObject`, not expressible as a static `Py_tp_base`),
  and populate the custom method descriptors via the `SetDictItem` accessor
  (Increment 2) — i.e. an abi3 overload of `PyVTKClass_Add` taking a `PyType_Spec*`
  + base. The number protocol's `Py_nb_*` slot decomposition for wrapped classes
  (the generated `Py%s_AsNumber` table) is the companion emitter still to add. So
  a wrapped module compiles its spec block but does not yet *import* under abi3;
  that is the runtime-glue work, bounded and identified here.

**(b) MEASUREMENT — real numbers (executor, cp313, minimal profile).**
- Scope of the wrapper compile: **85 Python modules, 1664 generated wrapper
  `.cxx` sources**, compiled (fvtk uses unity builds for wrappers) into **97
  unity wrapper TUs**.
- Per-unity-wrapper-TU true compile cost (ccache bypassed, measured on 4
  representative module unity-0 TUs): **1.10 / 1.48 / 1.53 / 1.36 s → ~1.35 s avg**.
- Total wrapper compilation per cp leg: **97 × ~1.35 s ≈ 131 CPU-seconds** (the
  C++ kit libs are already shared across legs via the ccache cross-leg fix, so
  this wrapper compile is the *entire* marginal per-leg cost).
- **Today (per-version wrappers):** the wheel matrix is 4 cp legs (cp311–cp314,
  post Increment 0). The wrapper `.so`s are version-tagged
  (`*.cpython-3XX-*.so`), so every leg recompiles all 97 unity TUs →
  **4 × 131 ≈ 524 CPU-seconds of wrapper compilation**.
- **With abi3 (single `cp3x-abi3` wheel):** wrappers compiled **once** →
  **~131 CPU-seconds**, and future CPython minors (cp315, …) need **no rebuild**
  at all (the stable-ABI `.so` is forward-compatible).
- **Savings: ~393 CPU-seconds, i.e. 3 of 4 legs' wrapper compilation eliminated
  (~75 % of the wrapper-compile cost), plus zero-cost support for every future
  CPython minor.** On the full (non-minimal) CoDim build the wrapper-TU count is
  ~2–3× larger, so the absolute saving scales up proportionally.
- **Wheel tag flip (the one-line product change, once the runtime glue lands):**
  `pyproject.toml` `build = "cp311-* cp312-* cp313-* cp314-*"` → `build = "cp311-*"`
  (single leg), and the wrapper SOABI tag in `ci/cibw/fvtk_backend.py:47`
  (`sysconfig.get_config_var("SOABI")`, currently `cpython-313-…`) becomes `abi3`
  so the one wheel is tagged `cp311-abi3-<plat>` and installs on cp311+.

**Full-generator rollout picks up at:** (1) the `Py%s_ClassNew`/`PyVTKClass_Add`
abi3 overload (`PyType_FromSpec` + runtime `tp_base` + `SetDictItem` dict
population) — this also closes the Increment-2 `tp_dict`-population residual; (2)
the wrapped-class number-protocol `Py_nb_*` slot emitter; (3) the enum
(`vtkWrapPythonEnum.c:187`) and special-type (`vtkWrapPythonType.c:664`)
emitters, same `PyType_Spec` pattern; (4) the remaining runtime-TU mechanical
casts + the `_PyType_Lookup`/`Py_HashPointer`/`FindGetSetDescriptor`/`tp_name`
sites enumerated in the Increment-2 entry. Then the wheel-tag flip above.


Validation context: executor host, warm ninja tree at `~/tmp/fvtk`, numeric
bit-exact suite (`tests/bitexact/`, 124 cases) vs stock VTK 9.6.2 + a new
wrapper-behavior parity gate (`test_wrapper_parity.py`). The abi3 leg is built
in an ISOLATED tree (`~/tmp/abi3-acd-afc6`, `-DFVTK_ABI3=ON`, minimal profile,
cp313) so it never disturbs the shared default-build tree other agents validate
against; the default leg is built there too (`build-default/`, FVTK_ABI3 OFF) to
confirm byte-identity.

### Increment 2 (heap-type crossing) — runtime type-definitions PORTED + buffer slots + abi3-aware gate — LANDED; runtime non-type tail DOCUMENTED

Decision taken (per the product go-ahead): cross the static→heap wall behind
`FVTK_ABI3`, accepting the single `__flags__` (HEAPTYPE=1/IMMUTABLETYPE=0)
divergence proven in the prior entry, keeping everything else bit-exact.

**What landed (compiles clean both legs; default byte-identical).**
- **All 7 hand-written runtime `PyTypeObject`s converted to `PyType_FromSpec`
  heap types behind `#if defined(Py_LIMITED_API)`**, default static defs kept
  verbatim in the `#else`:
  - `PyVTKReference_Type` + `PyVTKNumberReference_Type` +
    `PyVTKStringReference_Type` + `PyVTKTupleReference_Type` — incl. the full
    number protocol decomposed from the (limited-API-opaque) `PyNumberMethods`
    table into individual `Py_nb_*` slots, sequence→`Py_sq_*`, mapping→`Py_mp_*`,
    and the **buffer protocol via `Py_bf_getbuffer`/`Py_bf_releasebuffer`
    slots**; the three subclasses wired to the `reference` base via
    `PyType_FromSpecWithBases`. Built by `PyVTKReference_BuildTypes()` from
    `PyVTKAddFile_PyVTKExtras` (replacing the `PyType_Ready` calls under abi3).
  - `PyVTKNamespace_Type`, `PyVTKTemplate_Type` (PyModule subclasses) — spec with
    `Py_tp_base = &PyModule_Type`; their `_New` chains to the base
    `tp_new`/`tp_init` through new limited-API-safe accessors
    (`vtkPythonType_GetNew/GetInit`) under abi3, default path untouched.
  - `PyVTKMethodDescriptor_Type` — spec incl. `Py_tp_members`. CPython's internal
    `PyMethodDescrObject`/`PyDescrObject` structs and `PyDescr_TYPE/NAME` accessors
    are NOT in the limited API, so under abi3 a self-contained ABI-stable struct
    (PyObject_HEAD + d_type + d_name + d_method) reproduces the exact observable
    facts (`__objclass__`/`__name__`/`__doc__`, call, descriptor-get, repr);
    `T_OBJECT`/`READONLY` mapped to `Py_T_OBJECT_EX`/`Py_READONLY`. Built from
    `vtkPythonUtil::Initialize()` under abi3.
  - The `#define PyVTKXxx_Type (*PyVTKXxx_TypePtr)` header shim keeps every
    existing `&PyVTKXxx_Type` / `Py_TYPE(o)==&PyVTKXxx_Type` use-site
    byte-identical in the default build and correct (→ the pointer) under abi3.
- **`tp_*` write-side (B2-write) accessor layer.** `vtkPythonTypeAccess.h` gained
  `vtkPythonType_GetNew/GetInit/GetDealloc` (read) and
  `vtkPythonType_SetDictItem/DelDictItem` (write) — the latter route the runtime's
  type-dict mutations through `PyObject_SetAttrString`/`DelAttrString` on the
  (mutable) heap type under abi3, the identical `PyDict_SetItemString(tp->tp_dict,…)`
  in the default build. **Also FIXED a latent bug in the inc-1 scaffolding**:
  `vtkPythonType_GetDict`'s limited-API branch called `PyType_GetDict`, which is
  NOT in the limited API (it is a regular-API 3.12 addition) — re-routed to a
  `__dict__` getattr (mappingproxy) so the shim actually compiles under abi3.
- **Buffer functions exported under abi3.** `PyVTKObject_AsBuffer_GetBuffer/
  ReleaseBuffer` get external linkage (and header decls) under abi3 so the
  generator's `PyType_Spec` (Increment 3) can wire them as `Py_bf_*` slots; the
  static `PyBufferProcs PyVTKObject_AsBuffer` and its header `extern` are guarded
  out (the struct is opaque under the limited API). Default build unchanged.
- **The parity gate is now abi3-aware.** `compare_parity()` accepts the
  HEAPTYPE/IMMUTABLETYPE flip *only* in the heap-vs-static direction when
  `BITEXACT_ABI3=1`, and reports EVERY other divergence (including a flag flipping
  the wrong way) as a failure. A new positive test `test_abi3_heaptypes_in_effect`
  asserts under abi3 that every probed wrapped/reference type ACTUALLY became a
  heap type (catches a silent non-conversion). Self-tested; the default
  (static-build) gate still passes 1/1 against the proven default backends.

**Validation.**
- DEFAULT leg (FVTK_ABI3 OFF) in the isolated tree: `WrappingPythonCore` rebuilds
  clean, all 13 TUs compile; parity gate passes against the shared proven
  backends (1 passed / abi3-positive test correctly skipped). Default codegen
  unchanged — every edit is either `Py_LIMITED_API`-guarded or a new inline that
  expands to the exact prior field access.
- ABI3 leg (FVTK_ABI3 ON, cp313 limited API): the 7 ported type-definitions and
  the buffer/dict accessor layer compile; the `PyBufferProcs`/`PyType_GetDict`
  blocker classes are eliminated (per-file error set dropped from 11 files to 6).

**STOP / documented residual — the runtime NON-type-definition tail.** A fully
importing abi3 `WrappingPythonCore` is NOT yet reachable: beyond the type
*definitions* (now done), the surrounding runtime code carries limited-API
incompatibilities that are the feasibility doc's "Phase 1 runtime port" and have
been pinned with compiler evidence on the cp313 limited-API build:
- **`Py_INCREF`/`Py_DECREF`/`Py_VISIT` on typed struct pointers** (`PyVTKReference*`,
  `PyVTKObject*`, `PyTypeObject*`): under the limited API these are inline funcs
  taking `PyObject*`, so each call needs an explicit `(PyObject*)` cast. Pervasive
  (dozens of sites across every runtime TU) but purely mechanical and bit-exact
  (no-op cast in the default build). ~30 sites.
- **Private/unstable APIs with no limited-API equivalent:** `_PyType_Lookup`
  (PyVTKReference `__trunc__`/`__round__` lookup — replaceable by an attr lookup
  with subtly different semantics), `Py_HashPointer` (vtkPythonUtil VariantHash).
- **CPython-internal struct layout dependencies:** `vtkPythonUtil::
  FindGetSetDescriptor` reads `PyGetSetDescrObject`/`PyDescrObject` fields and
  `PyLong_Type` directly; these structs are opaque under the limited API. This one
  needs a behavioral redesign (it walks built-in getset descriptors), not a cast.
- **The central wrapping path `PyVTKClass_Add` lazy-populates a generated type's
  `tp_dict`** (`pytype->tp_dict = PyDict_New()` + raw inserts) with an idempotency
  guard of `tp_dict != nullptr`. Under abi3 heap types always own a dict and the
  slot is unwritable; the clean fix is to have the **generator emit the method
  dict via `Py_tp_methods`/`Py_tp_getset` spec slots** rather than post-hoc raw
  `tp_dict` writes — i.e. this belongs WITH the Increment-3 generator port, not
  the runtime, and is co-designed there. The `__override__` set/del sites are
  already routed through the new `SetDictItem`/`DelDictItem` accessors.

Net: the parity-risk *core* of crossing the wall — turning the hand-written types
into heap types while proving (gate) that only `__flags__` moves — is landed and
instrumented. The remaining runtime tail is mechanical casts + a handful of
private-API/internal-struct sites; it is bounded and enumerated above, and the
`tp_dict`-population piece is deliberately deferred to ride with the generator
port where it has a clean (`Py_tp_methods`-slot) form.

### Increment 2 — finish B2 accessor migration in the runtime + locate the heap-type parity wall — LANDED (hygiene) / WALL DOCUMENTED

Two parts: (1) a safe, independently-valuable continuation of the Increment-1
accessor migration, and (2) the empirical pin-down of the parity wall that
gates the *next* step (the static→heap PyType_FromSpec conversion). Per the
mandate ("if a divergence is unavoidable, STOP, document it precisely, and
leave increments 0–1 in place"), the heap-type conversion is deliberately NOT
landed; the proven wall below is why.

- **Hygiene that landed.** Migrated the remaining direct-`tp_*` read ladders in
  the runtime to the accessor layer:
  - `vtkPythonOverload.cxx` — **six** copies of the `tp_base` superclass-walk
    version ladder (3 init + 3 loop, across the 'V'/'W'/'&' arg-matching arms)
    collapsed to `vtkPythonType_GetBase()`.
  - `PyVTKSpecialObject.cxx` (`PyVTKSpecialObject_Repr`) — the `tp_base`/`tp_str`
    base-walk-to-find-`__str__` ladder collapsed; added a
    `vtkPythonType_GetStr()` accessor (same byte-identical/limited split).
  - Net `-40`ish lines of duplicated `#if PY_VERSION_HEX >= 0x030A0000 … #else …`
    boilerplate; further shrinks the B2 surface. Byte-identical default-build
    codegen (cp311+ already emitted `PyType_GetSlot` at all these sites).
  - Parity gate extended to probe the `reference` (mutable proxy) runtime helper
    type — `__flags__` (heaptype/immutabletype), mro, repr, set/get round-trip —
    so the upcoming heap-type change is observable here.
- **Validation.** Executor full build clean (vtkCommonCore relinked). **125
  passed / 0 failed** (124 numeric + parity gate, now incl. the `reference`
  facts). `reference_flag_heaptype=false`, `reference_flag_immutabletype=true`
  vs stock — i.e. still a STATIC type, matching stock 9.6.2 exactly.

- **THE PARITY WALL (evidence-backed, gates Increment 3 product-flip).**
  `PyType_FromSpec` — the *only* limited-API way to create a type — **always**
  produces a heap type. Compiled and ran the exact construct against the real
  CPython 3.13 limited-API headers/runtime on the executor:

  ```c
  #define Py_LIMITED_API 0x030b0000
  #include <Python.h>
  static PyType_Slot slots[]={{0,0}};
  static PyType_Spec spec={"probe.Thing",0,0,Py_TPFLAGS_DEFAULT,slots};
  // PyType_FromSpec(&spec) -> __flags__ :  HEAPTYPE=1  IMMUTABLETYPE=0
  ```

  Stock VTK 9.6.2's wrapped classes and runtime helper types are **static** —
  `HEAPTYPE=0  IMMUTABLETYPE=1` (confirmed by the parity gate for vtkObjectBase,
  vtkObject, vtkDoubleArray, vtkPoints, vtkPolyData, and `reference`). So under
  any abi3 (`Py_LIMITED_API`) build, **every** wrapped type's
  `type(x).__flags__` necessarily differs from stock in the HEAPTYPE (1<<9) and
  IMMUTABLETYPE (1<<8) bits. There is no limited-API mechanism to produce a
  static type; this is intrinsic, not a fixable shim gap.

  **Consequence for the bit-exact mandate:** the abi3 *wheel* cannot be
  byte-for-byte on `__flags__`. Everything else the parity gate checks (mro,
  isinstance, identity, repr shape, the numpy zero-copy buffer protocol,
  weakref, instance `__dict__`) is reproducible under heap types; only the two
  flag bits are not. Whether that single divergence is acceptable is a
  **product decision** (does any fvtk consumer branch on `__flags__` /
  `Py_TPFLAGS_HEAPTYPE`?), not a coding one — which is exactly why the
  heap-type conversion is held here behind that decision rather than shipped.

- **What this means for the ladder.** Increments 0–2 are all bit-exact and
  independently valuable *today* (CI trim; B2 accessor hygiene; the parity
  gate + the wall pinned with evidence). The static→heap port (former
  "Increment 2/3") is **ready to implement behind `FVTK_ABI3`** — the accessor
  layer + parity gate are the scaffolding for it — but is gated on accepting
  the `__flags__` divergence. The accessor `Py_LIMITED_API` branches and the
  parity gate are in place so that, the day that decision is made, the heap
  port is a contained change with an instrument already watching it.

### Increment 1 — `tp_*` accessor shim layer (API hygiene, no-op today) — LANDED

- **What landed.** New header `Wrapping/PythonCore/vtkPythonTypeAccess.h` with
  inline read accessors `vtkPythonType_GetBase()` / `vtkPythonType_GetDict()`.
  Under the default (non-limited) build they expand to exactly the access the
  runtime already used — `PyType_GetSlot(tp, Py_tp_base)` for `tp_base` (the
  form the code already used behind a `PY_VERSION_HEX >= 0x030A0000` ladder at
  every site) and the plain `tp->tp_dict` field read for `tp_dict`. Under
  `Py_LIMITED_API` they route to `PyType_GetSlot` / `PyType_GetDict`.
- **Sites migrated** (reads only — borrowed-slot reads; the few tp_* *writes*,
  e.g. lazy `tp_dict = PyDict_New()`, are deliberately left for the heap-type
  increment): `PyVTKObject.cxx` (3 sites incl. one inline version-ladder
  collapsed), `vtkPythonUtil.cxx` (4 sites, collapsing **three** inline
  `#if PY_VERSION_HEX` ladders into one accessor). Net `-16` lines, removes 3
  copies of the version-gated base-walk boilerplate (B2 blocker shrinks).
- **Bit-exact / API / perf delta.** API hygiene win + small dead-code removal;
  byte-for-byte behavior preserved. Default-build codegen is equivalent (the
  migrated `tp_base` sites already emitted `PyType_GetSlot` on the cp311+
  matrix; `tp_dict` stays a direct field read). No runtime-perf change; no
  numeric change.
- **Validation.** Executor full build clean (WrappingPythonCore relinked,
  `vtkCommonCore.*.so` rebuilt). **125 passed / 0 failed** = 124 numeric
  bit-exact cases + the new parity gate. Parity probe confirms vs stock 9.6.2:
  type `__flags__` (HEAPTYPE=0 / IMMUTABLETYPE=1 — static types preserved), mro
  / isinstance / issubclass, repr shape, **numpy zero-copy buffer protocol
  (byte-identical values + shared-memory mutation, the B5 risk surface)**,
  weakref, instance `__dict__`, `override` presence — all identical (modulo the
  intentional `vtkmodules`→`fvtk` package rename, normalized out).
- **New safety net.** `tests/bitexact/wrapper_parity.py` +
  `test_wrapper_parity.py` — the wrapper-behavior parity gate, to be run every
  subsequent increment. This is the instrument that will catch the heap-type
  divergence in Increment 2.
- **Next pickup.** Increment 2: convert the ~8 static `PyTypeObject` defs in
  `Wrapping/PythonCore` to `PyType_FromSpec` heap types behind a compat shim,
  watching `flag_heaptype::*` in the parity gate — if it flips to `True`, that
  is the documented heap-vs-static parity wall.

### Increment 0 — cibuildwheel matrix trim (CI-time win) — LANDED

`pyproject.toml`: `build` selector `cp39…cp313` → `cp311 cp312 cp313 cp314`
(drop two EOL/near-EOL legs, add cp314; `requires-python` → `>=3.11`). 5 cp
legs → 4 = immediate CI-time reduction; cp311 is the abi3 floor. Pure packaging
metadata, no compiled-core or numeric change. Commit standalone.

---


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

**Original recommendation (superseded by the ladder above):** "do NOT pursue
full abi3 now." That still holds for the *final product flip* (an abi3 wheel
can't be byte-for-byte on `type(x).__flags__` — see the **parity wall** in
Increment 2's status entry, now pinned with executor evidence). What changed is
the *approach*: rather than defer wholesale, the work is being landed as the
bit-exact, independently-valuable **increment ladder** at the top of this doc
(0: CI trim; 1: accessor shim + parity gate; 2: finish B2 hygiene + locate the
wall). Each is a net win on its own; the heap-type port that crosses the wall
is the only piece held, gated on the `__flags__`-divergence product decision.
The `FVTK_ABI3` lever keeps the remaining worklist measurable.

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
