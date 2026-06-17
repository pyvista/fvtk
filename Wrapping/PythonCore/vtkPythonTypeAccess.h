// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
/*-----------------------------------------------------------------------
  vtkPythonTypeAccess.h — accessor shims for PyTypeObject "tp_*" fields.

  The Python wrapper runtime reads a handful of PyTypeObject slots directly
  (tp_dict, tp_base, ...). Under the default (non-limited) CPython build a
  PyTypeObject is a complete struct, so a direct field read is legal and is
  what VTK has always done. Under Py_LIMITED_API (the stable ABI, abi3) the
  PyTypeObject is an *opaque* incomplete type and the only sanctioned way to
  read a slot is PyType_GetSlot() / PyType_GetDict().

  These inline accessors express the read in one place so the runtime can be
  compiled either way. CRITICAL CONTRACT: under the default build (the only
  build fvtk currently ships) every accessor MUST expand to the *exact* same
  field access it replaces, so the emitted object code is byte-identical and
  this is a pure API-hygiene no-op. The Py_LIMITED_API branch is dormant
  today; it exists so the abi3 port can flip it on without re-touching every
  call site.

  Use these for *reads* of borrowed slot values. Writes to tp_* on a live
  type (e.g. lazily creating tp_dict) are deliberately NOT shimmed here —
  they have no limited-API-safe equivalent on heap types and are handled at
  their (few) call sites when the abi3 port reaches them.

  Include after vtkPython.h / Python.h.
-----------------------------------------------------------------------*/
#ifndef vtkPythonTypeAccess_h
#define vtkPythonTypeAccess_h

// VTK_ABI3_LIMITED is the single switch: it is defined exactly when the
// translation unit is being compiled against the Python limited API. fvtk's
// FVTK_ABI3 cmake lever injects Py_LIMITED_API, which CPython's headers turn
// into the opaque-PyTypeObject world this shim guards against.
#if defined(Py_LIMITED_API)
#define VTK_ABI3_LIMITED 1
#else
#define VTK_ABI3_LIMITED 0
#endif

// ---------------------------------------------------------------------------
// PyMemberDef + Py_T_*/Py_READONLY stable-ABI compatibility (limited-API floor
// below 3.12).
//
// The abi3 wrapper port carries each wrapped type's per-instance dict/weakref
// layout in a PyType_Spec via a Py_tp_members slot of synthetic
// "__dictoffset__"/"__weaklistoffset__" PyMemberDef entries (Py_T_PYSSIZET /
// Py_READONLY), and PyVTKMethodDescriptor uses a Py_T_OBJECT_EX member. CPython
// only promoted `struct PyMemberDef` and the Py_-prefixed member type/flag
// constants into the *stable ABI* in 3.12 (gh-93274). At a 3.11 limited-API
// floor (Py_LIMITED_API == 0x030b0000, the fvtk default) the headers do not
// expose them, so the generated *Python.cxx and the runtime fail to compile
// ("PyMemberDef has incomplete type" / "Py_T_PYSSIZET not declared").
//
// These definitions are part of the STABLE ABI from 3.12 on — the struct layout
// and the numeric type/flag values are fixed and forward-compatible — so we
// reproduce them verbatim when (and only when) building against a limited-API
// floor older than 3.12 that omits them. On 3.12+ (or any non-limited build) the
// real header definitions are present and these blocks are skipped, keeping the
// emitted code byte-identical to the header-provided path. The values mirror
// CPython's Include/descrobject.h exactly. Must precede any wrapper/runtime use.
#if defined(Py_LIMITED_API) && Py_LIMITED_API + 0 < 0x030c0000
#ifndef Py_T_PYSSIZET
// Member type/flag constants (Include/descrobject.h). Only the three the abi3
// port references are needed; the rest of the table is intentionally omitted.
// Defining Py_T_PYSSIZET here also makes any later <structmember.h> that gates
// on it skip its (limited-API-excluded) redefinition.
#define Py_T_OBJECT_EX 16
#define Py_T_PYSSIZET 19
#define Py_READONLY 1
// struct PyMemberDef is absent from the <3.12 limited API; define the stable
// layout (gh-93274) so Py_tp_members slot tables compile. Guard with a sentinel
// so a co-included header that also provides it cannot trigger a redefinition.
#ifndef VTK_ABI3_PYMEMBERDEF_DEFINED
#define VTK_ABI3_PYMEMBERDEF_DEFINED 1
struct PyMemberDef
{
  const char* name;
  int type;
  Py_ssize_t offset;
  int flags;
  const char* doc;
};
#endif
#endif
#endif

VTK_ABI_NAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Read a type's tp_base (the single declared C/Python base). Borrowed ref.
//
// PyType_GetSlot(tp, Py_tp_base) was extended to work on *all* types (not just
// heap types) in CPython 3.10, and is the form the limited API mandates. fvtk's
// floor is now cp311, so the non-limited branch uses the same PyType_GetSlot
// call the runtime already used at its existing tp_base read sites — the legacy
// "tp->tp_base" direct read remains only for the <3.10 fallback (never compiled
// under the current matrix, kept for upstream parity). This keeps the migrated
// sites byte-identical to their prior inline PyType_GetSlot ladders.
static inline PyTypeObject* vtkPythonType_GetBase(PyTypeObject* tp)
{
#if VTK_ABI3_LIMITED || PY_VERSION_HEX >= 0x030A0000
  return reinterpret_cast<PyTypeObject*>(PyType_GetSlot(tp, Py_tp_base));
#else
  return tp->tp_base;
#endif
}

//------------------------------------------------------------------------------
// Read a type's tp_str (its __str__ slot function), as a function pointer.
// Same byte-identical/limited-API split as GetBase: PyType_GetSlot on the
// cp311+ matrix (the form the runtime already used), legacy field read only on
// the never-compiled <3.10 fallback.
static inline reprfunc vtkPythonType_GetStr(PyTypeObject* tp)
{
#if VTK_ABI3_LIMITED || PY_VERSION_HEX >= 0x030A0000
  return reinterpret_cast<reprfunc>(PyType_GetSlot(tp, Py_tp_str));
#else
  return tp->tp_str;
#endif
}

//------------------------------------------------------------------------------
// Read a type's tp_dict (its attribute dictionary). Borrowed ref under the
// default build; PyType_GetDict (3.12+) returns a *new* reference, so the
// limited-API branch is only valid where the caller treats the result as
// borrowed for the duration of a single use (all current fvtk readers do —
// they immediately pass it to PyDict_GetItem*/SetItemString without storing
// it). Limited-API correctness of the new-ref leak is a port-time concern;
// today this branch is never compiled.
static inline PyObject* vtkPythonType_GetDict(PyTypeObject* tp)
{
#if VTK_ABI3_LIMITED
  // PyType_GetDict is NOT part of the limited API (it is a regular-API addition
  // in 3.12). Under abi3 the type's attribute namespace is reached through the
  // object protocol: getattr "__dict__" returns a mappingproxy that wraps the
  // real tp_dict. Callers that only do read lookups (PyMapping_GetItemString /
  // membership) work against the proxy identically. The proxy is a *new* ref;
  // current fvtk readers use the result transiently (immediately look an item up
  // and drop it), so this matches the borrowed-slot lifetime in practice.
  return PyObject_GetAttrString(reinterpret_cast<PyObject*>(tp), "__dict__");
#else
  return tp->tp_dict;
#endif
}

//------------------------------------------------------------------------------
// Set name->value in a type's attribute namespace. Under the default build this
// is a raw tp_dict insertion (what the runtime historically did); under abi3 the
// type is a (mutable) heap type and the limited-API-safe equivalent is a setattr
// through the type object, which lands in the same tp_dict and triggers the same
// type-modified bookkeeping. Returns 0 on success, -1 on error.
static inline int vtkPythonType_SetDictItem(PyTypeObject* tp, const char* name, PyObject* value)
{
#if VTK_ABI3_LIMITED
  return PyObject_SetAttrString(reinterpret_cast<PyObject*>(tp), name, value);
#else
  return PyDict_SetItemString(tp->tp_dict, name, value);
#endif
}

//------------------------------------------------------------------------------
// Delete name from a type's attribute namespace (no error if absent under the
// default build path's existing semantics handled by the caller). Returns 0 on
// success, -1 with an exception set if the attribute did not exist.
static inline int vtkPythonType_DelDictItem(PyTypeObject* tp, const char* name)
{
#if VTK_ABI3_LIMITED
  return PyObject_DelAttrString(reinterpret_cast<PyObject*>(tp), name);
#else
  return PyDict_DelItemString(tp->tp_dict, name);
#endif
}


//------------------------------------------------------------------------------
// Read a type's tp_new (its allocator/constructor slot), as a function pointer.
// Used by the PyModule-subclass runtime types (namespace/template) that chain to
// their base's tp_new/tp_init. Same byte-identical/limited split as GetBase.
static inline newfunc vtkPythonType_GetNew(PyTypeObject* tp)
{
#if VTK_ABI3_LIMITED || PY_VERSION_HEX >= 0x030A0000
  return reinterpret_cast<newfunc>(PyType_GetSlot(tp, Py_tp_new));
#else
  return tp->tp_new;
#endif
}

//------------------------------------------------------------------------------
// Read a type's tp_init slot, as a function pointer.
static inline initproc vtkPythonType_GetInit(PyTypeObject* tp)
{
#if VTK_ABI3_LIMITED || PY_VERSION_HEX >= 0x030A0000
  return reinterpret_cast<initproc>(PyType_GetSlot(tp, Py_tp_init));
#else
  return tp->tp_init;
#endif
}

//------------------------------------------------------------------------------
// Read a type's tp_dealloc slot, as a function pointer.
static inline destructor vtkPythonType_GetDealloc(PyTypeObject* tp)
{
#if VTK_ABI3_LIMITED || PY_VERSION_HEX >= 0x030A0000
  return reinterpret_cast<destructor>(PyType_GetSlot(tp, Py_tp_dealloc));
#else
  return tp->tp_dealloc;
#endif
}

//------------------------------------------------------------------------------
// Look up `name` in a type's MRO without triggering descriptor binding, the way
// the private _PyType_Lookup does (borrowed ref / NULL, no exception set on
// miss). Used for fetching numeric dunder slots (__trunc__/__round__) off a
// built-in type. Default build keeps the exact _PyType_Lookup call; abi3 (no
// private API) falls back to a getattr on the type object — for the built-in
// numeric types these dunders resolve to the same callable taking the instance.
// NOTE the abi3 branch returns a NEW reference and may set an exception on miss;
// the two call sites clear it / decref appropriately under abi3.
static inline PyObject* vtkPythonType_LookupMethod(PyTypeObject* tp, PyObject* name)
{
#if VTK_ABI3_LIMITED
  PyObject* m = PyObject_GetAttr(reinterpret_cast<PyObject*>(tp), name);
  if (!m)
  {
    PyErr_Clear();
  }
  return m;
#else
  return _PyType_Lookup(tp, name);
#endif
}

//------------------------------------------------------------------------------
// Copy every (key, value) from a plain dict into a type's attribute namespace.
// The generator populates constants/enum-members into a temporary dict using the
// existing emitters (which call PyDict_SetItemString on a PyObject* dict); under
// the default build that dict IS the type's tp_dict, but a heap type's dict is
// not directly writable, so under abi3 the generator builds a standalone dict and
// this routes each entry through SetDictItem. Returns 0 on success, -1 on error.
static inline int vtkPythonType_MergeIntoTypeDict(PyTypeObject* tp, PyObject* src)
{
  int rc = 0;
  Py_ssize_t pos = 0;
  PyObject *key, *value;
  while (PyDict_Next(src, &pos, &key, &value))
  {
    const char* name = PyUnicode_AsUTF8AndSize(key, nullptr);
    if (name == nullptr || vtkPythonType_SetDictItem(tp, name, value) != 0)
    {
      rc = -1;
      break;
    }
  }
  return rc;
}

#if VTK_ABI3_LIMITED
//------------------------------------------------------------------------------
// abi3 heap-type construction helper. Under Py_LIMITED_API the only way to make
// a type is PyType_FromSpec, which yields a *heap* type. This wraps the common
// build-and-ready sequence: call PyType_FromSpec, then (if a base tuple is
// needed for multiple/explicit bases) PyType_FromModuleAndSpec is avoided —
// fvtk's runtime types each have a single base expressed via the Py_tp_base
// slot inside the spec, so plain PyType_FromSpec suffices. The returned object
// is a *new* strong reference (the static-type world held these as file-scope
// globals with effectively static lifetime; under abi3 we leak one ref per
// runtime type at module init, matching that "lives forever" semantics).
static inline PyTypeObject* vtkPythonType_FromSpec(PyType_Spec* spec)
{
  return reinterpret_cast<PyTypeObject*>(PyType_FromSpec(spec));
}
#endif

VTK_ABI_NAMESPACE_END

#endif
// VTK-HeaderTest-Exclude: vtkPythonTypeAccess.h
