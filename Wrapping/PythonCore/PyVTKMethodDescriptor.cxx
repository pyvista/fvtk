// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause

#include "PyVTKMethodDescriptor.h"
#include "vtkABINamespace.h"
#include "vtkPythonTypeAccess.h"
#include "vtkPythonUtil.h"

#include <structmember.h> // a python header

// Silence warning like
// "dereferencing type-punned pointer will break strict-aliasing rules"
// it happens because this kind of expression: (long *)&ptr
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif

#if defined(Py_LIMITED_API)
// Under the limited API, CPython's PyDescrObject / PyMethodDescrObject structs
// and the PyDescr_TYPE/PyDescr_NAME accessors are not exposed (descrobject.h is
// excluded from the stable ABI). The custom VTK method descriptor doesn't need
// to *be* a CPython method descriptor — it only needs an object with d_type /
// d_name / d_method fields, a getattro of PyObject_GenericGetAttr, and members
// exposing __objclass__ / __name__. So define a self-contained, ABI-stable
// layout that reproduces exactly those observable facts. PyObject_HEAD is part
// of the stable ABI; offsetof on a user struct is plain C.
struct PyMethodDescrObject
{
  PyObject_HEAD
  PyTypeObject* d_type;  // __objclass__
  PyObject* d_name;      // __name__ (interned)
  PyMethodDef* d_method; // the wrapped method
};
struct PyDescrObject
{
  PyObject_HEAD
  PyTypeObject* d_type;
  PyObject* d_name;
};
#undef PyDescr_TYPE
#undef PyDescr_NAME
#define PyDescr_TYPE(x) (((PyMethodDescrObject*)(x))->d_type)
#define PyDescr_NAME(x) (((PyMethodDescrObject*)(x))->d_name)
// The limited API spells the member type/flags Py_T_OBJECT_EX / Py_READONLY.
#ifndef T_OBJECT
#define T_OBJECT Py_T_OBJECT_EX
#endif
#ifndef READONLY
#define READONLY Py_READONLY
#endif
#else
// Required for Python 2.5 through Python 2.7
#ifndef PyDescr_TYPE
#define PyDescr_TYPE(x) (((PyDescrObject*)(x))->d_type)
#define PyDescr_NAME(x) (((PyDescrObject*)(x))->d_name)
#endif
#endif

//------------------------------------------------------------------------------
// C API

PyObject* PyVTKMethodDescriptor_New(PyTypeObject* pytype, PyMethodDef* meth)
{
#if defined(Py_LIMITED_API)
  // abi3: ensure the heap type exists before allocating from it.
  PyVTKMethodDescriptor_BuildType();
#endif
  PyMethodDescrObject* descr =
    (PyMethodDescrObject*)PyType_GenericAlloc(&PyVTKMethodDescriptor_Type, 0);

  if (descr)
  {
    Py_XINCREF((PyObject*)pytype);
    PyDescr_TYPE(descr) = pytype;
    PyDescr_NAME(descr) = PyUnicode_InternFromString(meth->ml_name);
    descr->d_method = meth;

    if (!PyDescr_NAME(descr))
    {
      Py_DECREF(descr);
      descr = nullptr;
    }
  }

  return (PyObject*)descr;
}

//------------------------------------------------------------------------------
// Object protocol

static void PyVTKMethodDescriptor_Delete(PyObject* ob)
{
  PyMethodDescrObject* descr = (PyMethodDescrObject*)ob;
  PyObject_GC_UnTrack(descr);
  Py_XDECREF((PyObject*)PyDescr_TYPE(descr));
  Py_XDECREF(PyDescr_NAME(descr));
  PyObject_GC_Del(descr);
}

static PyObject* PyVTKMethodDescriptor_Repr(PyObject* ob)
{
  PyMethodDescrObject* descr = (PyMethodDescrObject*)ob;
  return PyUnicode_FromFormat("<method \'%U\' of \'%s\' objects>", PyDescr_NAME(descr),
    vtkPythonUtil::GetTypeName(PyDescr_TYPE(descr)));
}

static int PyVTKMethodDescriptor_Traverse(PyObject* ob, visitproc visit, void* arg)
{
  PyMethodDescrObject* descr = (PyMethodDescrObject*)ob;
  Py_VISIT((PyObject*)PyDescr_TYPE(descr));
  return 0;
}

static PyObject* PyVTKMethodDescriptor_Call(PyObject* ob, PyObject* args, PyObject* kwds)
{
  PyMethodDescrObject* descr = (PyMethodDescrObject*)ob;
  PyMethodDef* meth = descr->d_method;

  // "self" for a method descriptor is the owning type; the wrapper functions
  // recover the real instance from args[0] (the unbound calling convention,
  // see vtkPythonArgs::GetSelfFromFirstArg). This is exactly the same self+args
  // pairing PyCFunction_New(...) + PyObject_Call(...) used to forward, so the
  // observable behavior is identical -- we only elide the per-call temporary
  // PyCFunction object (a GC-tracked heap alloc+free on every wrapped call).
  // All wrapped methods are emitted with either METH_VARARGS or
  // METH_VARARGS|METH_KEYWORDS (see vtkWrapPythonMethodDef.c), so dispatch on
  // METH_KEYWORDS covers every case.
  PyObject* self = (PyObject*)PyDescr_TYPE(descr);

  if (meth->ml_flags & METH_KEYWORDS)
  {
    return ((PyCFunctionWithKeywords)(void (*)(void))meth->ml_meth)(self, args, kwds);
  }

  // A METH_VARARGS-only method rejects keyword arguments, matching the
  // TypeError that PyObject_Call(PyCFunction, ...) would have raised.
  if (kwds != nullptr && PyDict_Size(kwds) != 0)
  {
    PyErr_Format(PyExc_TypeError, "%U() takes no keyword arguments", PyDescr_NAME(descr));
    return nullptr;
  }

  return meth->ml_meth(self, args);
}

static PyObject* PyVTKMethodDescriptor_Get(PyObject* self, PyObject* obj, PyObject*)
{
  PyMethodDescrObject* descr = (PyMethodDescrObject*)self;

  if (obj == nullptr)
  {
    // If no object to bind to, return the descriptor itself
    Py_INCREF(self);
    return self;
  }

  if (PyObject_TypeCheck(obj, PyDescr_TYPE(descr)))
  {
    // Bind the method to the object
    return PyCFunction_New(descr->d_method, obj);
  }

  PyErr_Format(PyExc_TypeError, "descriptor '%U' for '%s' objects doesn't apply to '%s' object",
    PyDescr_NAME(descr), vtkPythonUtil::GetTypeName(PyDescr_TYPE(descr)),
    vtkPythonUtil::GetTypeNameForObject(obj));

  return nullptr;
}

static PyObject* PyVTKMethodDescriptor_GetDoc(PyObject* ob, void*)
{
  PyMethodDescrObject* descr = (PyMethodDescrObject*)ob;

  if (descr->d_method->ml_doc == nullptr)
  {
    Py_INCREF(Py_None);
    return Py_None;
  }

  return PyUnicode_FromString(descr->d_method->ml_doc);
}

#if PY_VERSION_HEX >= 0x03070000
#define pystr(x) x
#else
#define pystr(x) const_cast<char*>(x)
#endif

static PyGetSetDef PyVTKMethodDescriptor_GetSet[] = {
  { pystr("__doc__"), PyVTKMethodDescriptor_GetDoc, nullptr, nullptr, nullptr },
  { nullptr, nullptr, nullptr, nullptr, nullptr }
};

static PyMemberDef PyVTKMethodDescriptor_Members[] = {
  { pystr("__objclass__"), T_OBJECT, offsetof(PyDescrObject, d_type), READONLY, nullptr },
  { pystr("__name__"), T_OBJECT, offsetof(PyDescrObject, d_name), READONLY, nullptr },
  { nullptr, 0, 0, 0, nullptr }
};

#ifdef VTK_PYTHON_NEEDS_DEPRECATION_WARNING_SUPPRESSION
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

//------------------------------------------------------------------------------
#if !defined(Py_LIMITED_API)
// clang-format off
PyTypeObject PyVTKMethodDescriptor_Type = {
  PyVarObject_HEAD_INIT(&PyType_Type, 0)
  "fvtk.vtkCommonCore.method_descriptor", // tp_name
  sizeof(PyMethodDescrObject),             // tp_basicsize
  0,                                       // tp_itemsize
  PyVTKMethodDescriptor_Delete,            // tp_dealloc
#if PY_VERSION_HEX >= 0x03080000
  //Prior to Py3.8, this member was a function pointer,
  //but as of Py3.8 it is an integer
  //(and therefore incompatible with nullptr).
  0,                                       // tp_vectorcall_offset
#else
  nullptr,                                 // tp_print
#endif
  nullptr,                                 // tp_getattr
  nullptr,                                 // tp_setattr
  nullptr,                                 // tp_compare
  PyVTKMethodDescriptor_Repr,              // tp_repr
  nullptr,                                 // tp_as_number
  nullptr,                                 // tp_as_sequence
  nullptr,                                 // tp_as_mapping
  nullptr,                                 // tp_hash
  PyVTKMethodDescriptor_Call,              // tp_call
  nullptr,                                 // tp_string
  PyObject_GenericGetAttr,                 // tp_getattro
  nullptr,                                 // tp_setattro
  nullptr,                                 // tp_as_buffer
  Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
    Py_TPFLAGS_METHOD_DESCRIPTOR,          // tp_flags
  nullptr,                                 // tp_doc
  PyVTKMethodDescriptor_Traverse,          // tp_traverse
  nullptr,                                 // tp_clear
  nullptr,                                 // tp_richcompare
  0,                                       // tp_weaklistoffset
  nullptr,                                 // tp_iter
  nullptr,                                 // tp_iternext
  nullptr,                                 // tp_methods
  PyVTKMethodDescriptor_Members,           // tp_members
  PyVTKMethodDescriptor_GetSet,            // tp_getset
  nullptr,                                 // tp_base
  nullptr,                                 // tp_dict
  PyVTKMethodDescriptor_Get,               // tp_descr_get
  nullptr,                                 // tp_descr_set
  0,                                       // tp_dictoffset
  nullptr,                                 // tp_init
  nullptr,                                 // tp_alloc
  nullptr,                                 // tp_new
  nullptr,                                 // tp_free
  nullptr,                                 // tp_is_gc
  nullptr,                                 // tp_bases
  nullptr,                                 // tp_mro
  nullptr,                                 // tp_cache
  nullptr,                                 // tp_subclasses
  nullptr,                                 // tp_weaklist
  VTK_WRAP_PYTHON_SUPPRESS_UNINITIALIZED };
// clang-format on
#else // Py_LIMITED_API: heap type via PyType_FromSpec

static PyType_Slot PyVTKMethodDescriptor_Slots[] = {
  { Py_tp_dealloc, (void*)PyVTKMethodDescriptor_Delete },
  { Py_tp_repr, (void*)PyVTKMethodDescriptor_Repr },
  { Py_tp_call, (void*)PyVTKMethodDescriptor_Call },
  { Py_tp_getattro, (void*)PyObject_GenericGetAttr },
  { Py_tp_traverse, (void*)PyVTKMethodDescriptor_Traverse },
  { Py_tp_members, (void*)PyVTKMethodDescriptor_Members },
  { Py_tp_getset, (void*)PyVTKMethodDescriptor_GetSet },
  { Py_tp_descr_get, (void*)PyVTKMethodDescriptor_Get },
  { 0, nullptr }
};
static PyType_Spec PyVTKMethodDescriptor_Spec = {
  "fvtk.vtkCommonCore.method_descriptor", sizeof(PyMethodDescrObject), 0,
  Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_METHOD_DESCRIPTOR,
  PyVTKMethodDescriptor_Slots
};

// Backing pointer for the `#define PyVTKMethodDescriptor_Type (*ptr)` shim.
PyTypeObject* PyVTKMethodDescriptor_TypePtr = nullptr;

int PyVTKMethodDescriptor_BuildType()
{
  if (PyVTKMethodDescriptor_TypePtr)
  {
    return 0;
  }
  PyVTKMethodDescriptor_TypePtr = vtkPythonType_FromSpec(&PyVTKMethodDescriptor_Spec);
  return PyVTKMethodDescriptor_TypePtr ? 0 : -1;
}
#endif // Py_LIMITED_API
