// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
/*-----------------------------------------------------------------------
  The PyVTKNamespace was created in Nov 2014 by David Gobbi.

  This is a PyModule subclass for wrapping C++ namespaces.
-----------------------------------------------------------------------*/

#include "PyVTKNamespace.h"
#include "vtkABINamespace.h"
#include "vtkPythonTypeAccess.h"
#include "vtkPythonUtil.h"

// Silence warning like
// "dereferencing type-punned pointer will break strict-aliasing rules"
// it happens because this kind of expression: (long *)&ptr
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif

//------------------------------------------------------------------------------

static const char* PyVTKNamespace_Doc = "A python module that wraps a C++ namespace.\n";

//------------------------------------------------------------------------------
static void PyVTKNamespace_Delete(PyObject* op)
{
  // remove from the map so that there is no dangling reference
  vtkPythonUtil::RemoveNamespaceFromMap(op);
  // call the superclass destructor
#if PY_VERSION_HEX >= 0x030A0000
  PyTypeObject* type = Py_TYPE(op);
  PyTypeObject* base = (PyTypeObject*)PyType_GetSlot(type, Py_tp_base);
  if (base)
  {
    destructor dtor = (destructor)PyType_GetSlot(base, Py_tp_dealloc);
    dtor(op);
  }
#else
  PyVTKNamespace_Type.tp_base->tp_dealloc(op);
#endif
}

#ifdef VTK_PYTHON_NEEDS_DEPRECATION_WARNING_SUPPRESSION
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

//------------------------------------------------------------------------------
#if !defined(Py_LIMITED_API)
// clang-format off
PyTypeObject PyVTKNamespace_Type = {
  PyVarObject_HEAD_INIT(&PyType_Type, 0)
  "cvista.vtkCommonCore.namespace", // tp_name
  0,                  // tp_basicsize
  0,                  // tp_itemsize
  PyVTKNamespace_Delete, // tp_dealloc
#if PY_VERSION_HEX >= 0x03080000
  0,                  // tp_vectorcall_offset
#else
  nullptr,            // tp_print
#endif
  nullptr,            // tp_getattr
  nullptr,            // tp_setattr
  nullptr,            // tp_compare
  nullptr,            // tp_repr
  nullptr,            // tp_as_number
  nullptr,            // tp_as_sequence
  nullptr,            // tp_as_mapping
  nullptr,            // tp_hash
  nullptr,            // tp_call
  nullptr,            // tp_string
  nullptr,            // tp_getattro
  nullptr,            // tp_setattro
  nullptr,            // tp_as_buffer
  Py_TPFLAGS_DEFAULT, // tp_flags
  PyVTKNamespace_Doc, // tp_doc
  nullptr,            // tp_traverse
  nullptr,            // tp_clear
  nullptr,            // tp_richcompare
  0,                  // tp_weaklistoffset
  nullptr,            // tp_iter
  nullptr,            // tp_iternext
  nullptr,            // tp_methods
  nullptr,            // tp_members
  nullptr,            // tp_getset
  &PyModule_Type,     // tp_base
  nullptr,            // tp_dict
  nullptr,            // tp_descr_get
  nullptr,            // tp_descr_set
  0,                  // tp_dictoffset
  nullptr,            // tp_init
  nullptr,            // tp_alloc
  nullptr,            // tp_new
  nullptr,            // tp_free
  nullptr,            // tp_is_gc
  nullptr,            // tp_bases
  nullptr,            // tp_mro
  nullptr,            // tp_cache
  nullptr,            // tp_subclasses
  nullptr,            // tp_weaklist
  VTK_WRAP_PYTHON_SUPPRESS_UNINITIALIZED };
// clang-format on
#else // Py_LIMITED_API: heap type via PyType_FromSpec

static PyType_Slot PyVTKNamespace_Slots[] = {
  { Py_tp_dealloc, (void*)PyVTKNamespace_Delete },
  { Py_tp_doc, (void*)const_cast<char*>(PyVTKNamespace_Doc) },
  { Py_tp_base, (void*)&PyModule_Type },
  { 0, nullptr }
};
static PyType_Spec PyVTKNamespace_Spec = {
  "cvista.vtkCommonCore.namespace", 0, 0, Py_TPFLAGS_DEFAULT, PyVTKNamespace_Slots
};

// Backing pointer for the `#define PyVTKNamespace_Type (*ptr)` shim.
PyTypeObject* PyVTKNamespace_TypePtr = nullptr;

static int PyVTKNamespace_BuildType()
{
  if (PyVTKNamespace_TypePtr)
  {
    return 0;
  }
  PyVTKNamespace_TypePtr = vtkPythonType_FromSpec(&PyVTKNamespace_Spec);
  return PyVTKNamespace_TypePtr ? 0 : -1;
}
#endif // Py_LIMITED_API

//------------------------------------------------------------------------------
PyObject* PyVTKNamespace_New(const char* name)
{
  // first check to see if this namespace exists
  PyObject* self = vtkPythonUtil::FindNamespace(name);
  if (self)
  {
    Py_INCREF(self);
  }
  else
  {
#if defined(Py_LIMITED_API)
    // abi3: build the heap type (idempotent), then chain to PyModule_Type's
    // tp_new/tp_init through the limited-API-safe slot accessors.
    PyVTKNamespace_BuildType();
    PyTypeObject* nstype = &PyVTKNamespace_Type;
    PyTypeObject* base = vtkPythonType_GetBase(nstype);
    PyObject* empty = PyTuple_New(0);
    self = vtkPythonType_GetNew(base)(nstype, empty, nullptr);
    Py_DECREF(empty);
    PyObject* pyname = PyUnicode_FromString(name);
    PyObject* args = PyTuple_Pack(1, pyname);
    Py_DECREF(pyname);
    vtkPythonType_GetInit(base)(self, args, nullptr);
    Py_DECREF(args);
#else
    // make sure python has readied the type object
    PyType_Ready(&PyVTKNamespace_Type);
    // call the superclass new function
    PyObject* empty = PyTuple_New(0);
    self = PyVTKNamespace_Type.tp_base->tp_new(&PyVTKNamespace_Type, empty, nullptr);
    Py_DECREF(empty);
    // call the superclass init function
    PyObject* pyname = PyUnicode_FromString(name);
    PyObject* args = PyTuple_Pack(1, pyname);
    Py_DECREF(pyname);
    PyVTKNamespace_Type.tp_base->tp_init(self, args, nullptr);
    Py_DECREF(args);
#endif
    // remember the object for later reference
    vtkPythonUtil::AddNamespaceToMap(self);
  }
  return self;
}

//------------------------------------------------------------------------------
PyObject* PyVTKNamespace_GetDict(PyObject* self)
{
  return PyModule_GetDict(self);
}

//------------------------------------------------------------------------------
const char* PyVTKNamespace_GetName(PyObject* self)
{
  return PyModule_GetName(self);
}
