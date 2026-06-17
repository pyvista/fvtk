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
  return PyType_GetDict(tp);
#else
  return tp->tp_dict;
#endif
}

VTK_ABI_NAMESPACE_END

#endif
// VTK-HeaderTest-Exclude: vtkPythonTypeAccess.h
