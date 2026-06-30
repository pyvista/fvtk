// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
/*-----------------------------------------------------------------------
  The PyVTKReference was created in Sep 2010 by David Gobbi.

  This class is a proxy for immutable python objects like int, float,
  and string.  It allows these objects to be passed to VTK methods that
  require a reference.
-----------------------------------------------------------------------*/

#include "PyVTKReference.h"
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

static const char* PyVTKReference_Doc =
  "reference(value:int) -> reference\n"
  "reference(value:float) -> reference\n"
  "reference(value:str) -> reference\n"
  "reference(value:(int, ...)) -> reference\n"
  "\n"
  "A simple container that acts as a reference to its contents.\n\n"
  "This wrapper class is needed when a VTK method returns a value\n"
  "in an argument that has been passed by reference.  By calling\n"
  "\"m = vtk.reference(a)\" on a value, you can create a proxy to\n"
  "that value.  The value can be changed by calling \"m.set(b)\".\n";

//------------------------------------------------------------------------------
// helper method: make sure than an object is usable
static PyObject* PyVTKReference_CompatibleObject(PyObject* self, PyObject* opn)
{
  if (PyVTKReference_Check(opn))
  {
    if (self == nullptr || Py_TYPE(opn) == Py_TYPE(self))
    {
      // correct type, so return it
      opn = ((PyVTKReference*)opn)->value;
      Py_INCREF(opn);
      return opn;
    }
    // get contents, do further compatibility checks
    opn = ((PyVTKReference*)opn)->value;
  }

  // check if it is a string
  if (self == nullptr || Py_TYPE(self) == &PyVTKStringReference_Type)
  {
    if (PyUnicode_Check(opn) || PyBytes_Check(opn))
    {
      Py_INCREF(opn);
      return opn;
    }
  }

  // check if it is a tuple or list
  if (self == nullptr || Py_TYPE(self) == &PyVTKTupleReference_Type)
  {
    if (PyTuple_Check(opn) || PyList_Check(opn))
    {
      Py_INCREF(opn);
      return opn;
    }
  }

  // check if it is a number
  if (self == nullptr || Py_TYPE(self) == &PyVTKNumberReference_Type)
  {
    if (PyFloat_Check(opn) || PyLong_Check(opn))
    {
      Py_INCREF(opn);
      return opn;
    }

    // check if it has number protocol and suitable methods
#if PY_VERSION_HEX < 0x030A0000
    PyNumberMethods* nb = Py_TYPE(opn)->tp_as_number;
    if (nb)
#endif
    {
#if PY_VERSION_HEX >= 0x030A0000
      if (unaryfunc nb_index = (unaryfunc)PyType_GetSlot(Py_TYPE(opn), Py_nb_index))
#else
      if (unaryfunc nb_index = nb->nb_index)
#endif
      {
        opn = nb_index(opn);
        if (opn == nullptr || !PyLong_Check(opn))
        {
          PyErr_SetString(PyExc_TypeError, "nb_index should return integer object");
          return nullptr;
        }
        return opn;
      }
#if PY_VERSION_HEX >= 0x030A0000
      else if (unaryfunc nb_float = (unaryfunc)PyType_GetSlot(Py_TYPE(opn), Py_nb_float))
#else
      else if (unaryfunc nb_float = nb->nb_float)
#endif
      {
        opn = nb_float(opn);
        if (opn == nullptr || !PyFloat_Check(opn))
        {
          PyErr_SetString(PyExc_TypeError, "nb_float should return float object");
          return nullptr;
        }
        return opn;
      }
    }
  }

  // set error message according to required type
  const char* errmsg = "bad type";
  if (self == nullptr)
  {
    errmsg = "a numeric, string, or tuple object is required";
  }
  else if (Py_TYPE(self) == &PyVTKStringReference_Type)
  {
    errmsg = "a string object is required";
  }
  else if (Py_TYPE(self) == &PyVTKTupleReference_Type)
  {
    errmsg = "a tuple object is required";
  }
  else if (Py_TYPE(self) == &PyVTKNumberReference_Type)
  {
    errmsg = "a numeric object is required";
  }

  PyErr_SetString(PyExc_TypeError, errmsg);
  return nullptr;
}

//------------------------------------------------------------------------------
// methods from C

PyObject* PyVTKReference_GetValue(PyObject* self)
{
  if (PyVTKReference_Check(self))
  {
    return ((PyVTKReference*)self)->value;
  }
  else
  {
    PyErr_SetString(PyExc_TypeError, "a vtk.reference() object is required");
  }

  return nullptr;
}

int PyVTKReference_SetValue(PyObject* self, PyObject* val)
{
  if (PyVTKReference_Check(self))
  {
    PyObject** op = &((PyVTKReference*)self)->value;

    PyObject* result = PyVTKReference_CompatibleObject(self, val);
    Py_DECREF(val);
    if (result)
    {
      Py_DECREF(*op);
      *op = result;
      return 0;
    }
  }
  else
  {
    PyErr_SetString(PyExc_TypeError, "a vtk.reference() object is required");
  }

  return -1;
}

//------------------------------------------------------------------------------
// methods from python

static PyObject* PyVTKReference_Get(PyObject* self, PyObject* args)
{
  if (PyArg_ParseTuple(args, ":get"))
  {
    PyObject* ob = PyVTKReference_GetValue(self);
    Py_INCREF(ob);
    return ob;
  }

  return nullptr;
}

static PyObject* PyVTKReference_Set(PyObject* self, PyObject* args)
{
  PyObject* opn = nullptr;

  if (PyArg_ParseTuple(args, "O:set", &opn))
  {
    opn = PyVTKReference_CompatibleObject(self, opn);

    if (opn)
    {
      if (PyVTKReference_SetValue(self, opn) == 0)
      {
        Py_INCREF(Py_None);
        return Py_None;
      }
    }
  }

  return nullptr;
}

static PyObject* PyVTKReference_Trunc(PyObject* self, PyObject* args)
{
  PyObject* opn = nullptr;

  if (PyArg_ParseTuple(args, ":__trunc__", &opn))
  {
    PyObject* attr = PyUnicode_InternFromString("__trunc__");
    PyObject* ob = PyVTKReference_GetValue(self);
    PyObject* meth = vtkPythonType_LookupMethod(Py_TYPE(ob), attr);
    if (meth == nullptr)
    {
      PyErr_Format(PyExc_TypeError, "type %.100s doesn't define __trunc__ method",
        vtkPythonUtil::GetTypeNameForObject(ob));
      return nullptr;
    }
#if defined(Py_LIMITED_API)
    // abi3 LookupMethod returns a new ref (vs _PyType_Lookup's borrow); release it.
    PyObject* res = PyObject_CallFunction(meth, "O", ob);
    Py_DECREF(meth);
    return res;
#else
    return PyObject_CallFunction(meth, "O", ob);
#endif
  }

  return nullptr;
}

static PyObject* PyVTKReference_Round(PyObject* self, PyObject* args)
{
  PyObject* opn = nullptr;

  if (PyArg_ParseTuple(args, "|O:__round__", &opn))
  {
    PyObject* attr = PyUnicode_InternFromString("__round__");
    PyObject* ob = PyVTKReference_GetValue(self);
    PyObject* meth = vtkPythonType_LookupMethod(Py_TYPE(ob), attr);
    if (meth == nullptr)
    {
      PyErr_Format(PyExc_TypeError, "type %.100s doesn't define __round__ method",
        vtkPythonUtil::GetTypeNameForObject(ob));
      return nullptr;
    }
#if defined(Py_LIMITED_API)
    // abi3 LookupMethod returns a new ref (vs _PyType_Lookup's borrow); release it.
    PyObject* res = (opn ? PyObject_CallFunction(meth, "OO", ob, opn)
                         : PyObject_CallFunction(meth, "O", ob));
    Py_DECREF(meth);
    return res;
#else
    if (opn)
    {
      return PyObject_CallFunction(meth, "OO", ob, opn);
    }
    return PyObject_CallFunction(meth, "O", ob);
#endif
  }

  return nullptr;
}

static PyMethodDef PyVTKReference_Methods[] = { { "get", PyVTKReference_Get, METH_VARARGS,
                                                  "get() -> object\n\nGet the stored value." },
  { "set", PyVTKReference_Set, METH_VARARGS, "set(value:object) -> None\n\nSet the stored value." },
  { "__trunc__", PyVTKReference_Trunc, METH_VARARGS,
    "__trunc__() -> int\n\nReturns the Integral closest to x between 0 and x." },
  { "__round__", PyVTKReference_Round, METH_VARARGS,
    "__round__() -> int\n\nReturns the Integral closest to x, rounding half toward even.\n" },
  { nullptr, nullptr, 0, nullptr } };

//------------------------------------------------------------------------------
// Macros used for defining protocol methods

#define REFOBJECT_INTFUNC(prot, op)                                                                \
  static int PyVTKReference_##op(PyObject* ob)                                                     \
  {                                                                                                \
    ob = ((PyVTKReference*)ob)->value;                                                             \
    return Py##prot##_##op(ob);                                                                    \
  }

#define REFOBJECT_INTFUNC2(prot, op)                                                               \
  static int PyVTKReference_##op(PyObject* ob, PyObject* o)                                        \
  {                                                                                                \
    ob = ((PyVTKReference*)ob)->value;                                                             \
    return Py##prot##_##op(ob, o);                                                                 \
  }

#define REFOBJECT_SIZEFUNC(prot, op)                                                               \
  static Py_ssize_t PyVTKReference_##op(PyObject* ob)                                              \
  {                                                                                                \
    ob = ((PyVTKReference*)ob)->value;                                                             \
    return Py##prot##_##op(ob);                                                                    \
  }

#define REFOBJECT_INDEXFUNC(prot, op)                                                              \
  static PyObject* PyVTKReference_##op(PyObject* ob, Py_ssize_t i)                                 \
  {                                                                                                \
    ob = ((PyVTKReference*)ob)->value;                                                             \
    return Py##prot##_##op(ob, i);                                                                 \
  }

#define REFOBJECT_INDEXSETFUNC(prot, op)                                                           \
  static int PyVTKReference_##op(PyObject* ob, Py_ssize_t i, PyObject* o)                          \
  {                                                                                                \
    ob = ((PyVTKReference*)ob)->value;                                                             \
    return Py##prot##_##op(ob, i, o);                                                              \
  }

#define REFOBJECT_SLICEFUNC(prot, op)                                                              \
  static PyObject* PyVTKReference_##op(PyObject* ob, Py_ssize_t i, Py_ssize_t j)                   \
  {                                                                                                \
    ob = ((PyVTKReference*)ob)->value;                                                             \
    return Py##prot##_##op(ob, i, j);                                                              \
  }

#define REFOBJECT_SLICESETFUNC(prot, op)                                                           \
  static int PyVTKReference_##op(PyObject* ob, Py_ssize_t i, Py_ssize_t j, PyObject* o)            \
  {                                                                                                \
    ob = ((PyVTKReference*)ob)->value;                                                             \
    return Py##prot##_##op(ob, i, j, o);                                                           \
  }

#define REFOBJECT_UNARYFUNC(prot, op)                                                              \
  static PyObject* PyVTKReference_##op(PyObject* ob)                                               \
  {                                                                                                \
    ob = ((PyVTKReference*)ob)->value;                                                             \
    return Py##prot##_##op(ob);                                                                    \
  }

#define REFOBJECT_BINARYFUNC(prot, op)                                                             \
  static PyObject* PyVTKReference_##op(PyObject* ob1, PyObject* ob2)                               \
  {                                                                                                \
    if (PyVTKReference_Check(ob1))                                                                 \
    {                                                                                              \
      ob1 = ((PyVTKReference*)ob1)->value;                                                         \
    }                                                                                              \
    if (PyVTKReference_Check(ob2))                                                                 \
    {                                                                                              \
      ob2 = ((PyVTKReference*)ob2)->value;                                                         \
    }                                                                                              \
    return Py##prot##_##op(ob1, ob2);                                                              \
  }

#define REFOBJECT_INPLACEFUNC(prot, op)                                                            \
  static PyObject* PyVTKReference_InPlace##op(PyObject* ob1, PyObject* ob2)                        \
  {                                                                                                \
    PyVTKReference* ob = (PyVTKReference*)ob1;                                                     \
    PyObject* obn;                                                                                 \
    ob1 = ob->value;                                                                               \
    if (PyVTKReference_Check(ob2))                                                                 \
    {                                                                                              \
      ob2 = ((PyVTKReference*)ob2)->value;                                                         \
    }                                                                                              \
    obn = Py##prot##_##op(ob1, ob2);                                                               \
    if (obn)                                                                                       \
    {                                                                                              \
      ob->value = obn;                                                                             \
      Py_DECREF(ob1);                                                                              \
      Py_INCREF((PyObject*)ob);                                                                     \
      return (PyObject*)ob;                                                                        \
    }                                                                                              \
    return nullptr;                                                                                \
  }

#define REFOBJECT_INPLACEIFUNC(prot, op)                                                           \
  static PyObject* PyVTKReference_InPlace##op(PyObject* ob1, Py_ssize_t i)                         \
  {                                                                                                \
    PyVTKReference* ob = (PyVTKReference*)ob1;                                                     \
    PyObject* obn;                                                                                 \
    ob1 = ob->value;                                                                               \
    obn = Py##prot##_##op(ob1, i);                                                                 \
    if (obn)                                                                                       \
    {                                                                                              \
      ob->value = obn;                                                                             \
      Py_DECREF(ob1);                                                                              \
      Py_INCREF((PyObject*)ob);                                                                     \
      return (PyObject*)ob;                                                                        \
    }                                                                                              \
    return 0;                                                                                      \
  }

#define REFOBJECT_TERNARYFUNC(prot, op)                                                            \
  static PyObject* PyVTKReference_##op(PyObject* ob1, PyObject* ob2, PyObject* ob3)                \
  {                                                                                                \
    if (PyVTKReference_Check(ob1))                                                                 \
    {                                                                                              \
      ob1 = ((PyVTKReference*)ob1)->value;                                                         \
    }                                                                                              \
    if (PyVTKReference_Check(ob2))                                                                 \
    {                                                                                              \
      ob2 = ((PyVTKReference*)ob2)->value;                                                         \
    }                                                                                              \
    if (PyVTKReference_Check(ob2))                                                                 \
    {                                                                                              \
      ob3 = ((PyVTKReference*)ob3)->value;                                                         \
    }                                                                                              \
    return Py##prot##_##op(ob1, ob2, ob3);                                                         \
  }

#define REFOBJECT_INPLACETFUNC(prot, op)                                                           \
  static PyObject* PyVTKReference_InPlace##op(PyObject* ob1, PyObject* ob2, PyObject* ob3)         \
  {                                                                                                \
    PyVTKReference* ob = (PyVTKReference*)ob1;                                                     \
    PyObject* obn;                                                                                 \
    ob1 = ob->value;                                                                               \
    if (PyVTKReference_Check(ob2))                                                                 \
    {                                                                                              \
      ob2 = ((PyVTKReference*)ob2)->value;                                                         \
    }                                                                                              \
    if (PyVTKReference_Check(ob3))                                                                 \
    {                                                                                              \
      ob3 = ((PyVTKReference*)ob3)->value;                                                         \
    }                                                                                              \
    obn = Py##prot##_##op(ob1, ob2, ob3);                                                          \
    if (obn)                                                                                       \
    {                                                                                              \
      ob->value = obn;                                                                             \
      Py_DECREF(ob1);                                                                              \
      Py_INCREF((PyObject*)ob);                                                                     \
      return (PyObject*)ob;                                                                        \
    }                                                                                              \
    return nullptr;                                                                                \
  }

//------------------------------------------------------------------------------
// Number protocol

static int PyVTKReference_NonZero(PyObject* ob)
{
  ob = ((PyVTKReference*)ob)->value;
  return PyObject_IsTrue(ob);
}

REFOBJECT_BINARYFUNC(Number, Add)
REFOBJECT_BINARYFUNC(Number, Subtract)
REFOBJECT_BINARYFUNC(Number, Multiply)
REFOBJECT_BINARYFUNC(Number, Remainder)
REFOBJECT_BINARYFUNC(Number, Divmod)
REFOBJECT_TERNARYFUNC(Number, Power)
REFOBJECT_UNARYFUNC(Number, Negative)
REFOBJECT_UNARYFUNC(Number, Positive)
REFOBJECT_UNARYFUNC(Number, Absolute)
// NonZero
REFOBJECT_UNARYFUNC(Number, Invert)
REFOBJECT_BINARYFUNC(Number, Lshift)
REFOBJECT_BINARYFUNC(Number, Rshift)
REFOBJECT_BINARYFUNC(Number, And)
REFOBJECT_BINARYFUNC(Number, Or)
REFOBJECT_BINARYFUNC(Number, Xor)
// Coerce
REFOBJECT_UNARYFUNC(Number, Long)
REFOBJECT_UNARYFUNC(Number, Float)

REFOBJECT_INPLACEFUNC(Number, Add)
REFOBJECT_INPLACEFUNC(Number, Subtract)
REFOBJECT_INPLACEFUNC(Number, Multiply)
REFOBJECT_INPLACEFUNC(Number, Remainder)
REFOBJECT_INPLACETFUNC(Number, Power)
REFOBJECT_INPLACEFUNC(Number, Lshift)
REFOBJECT_INPLACEFUNC(Number, Rshift)
REFOBJECT_INPLACEFUNC(Number, And)
REFOBJECT_INPLACEFUNC(Number, Or)
REFOBJECT_INPLACEFUNC(Number, Xor)

REFOBJECT_BINARYFUNC(Number, FloorDivide)
REFOBJECT_BINARYFUNC(Number, TrueDivide)
REFOBJECT_INPLACEFUNC(Number, FloorDivide)
REFOBJECT_INPLACEFUNC(Number, TrueDivide)

REFOBJECT_UNARYFUNC(Number, Index)

//------------------------------------------------------------------------------
#if !defined(Py_LIMITED_API)
static PyNumberMethods PyVTKReference_AsNumber = {
  PyVTKReference_Add,                // nb_add
  PyVTKReference_Subtract,           // nb_subtract
  PyVTKReference_Multiply,           // nb_multiply
  PyVTKReference_Remainder,          // nb_remainder
  PyVTKReference_Divmod,             // nb_divmod
  PyVTKReference_Power,              // nb_power
  PyVTKReference_Negative,           // nb_negative
  PyVTKReference_Positive,           // nb_positive
  PyVTKReference_Absolute,           // nb_absolute
  PyVTKReference_NonZero,            // nb_nonzero
  PyVTKReference_Invert,             // nb_invert
  PyVTKReference_Lshift,             // nb_lshift
  PyVTKReference_Rshift,             // nb_rshift
  PyVTKReference_And,                // nb_and
  PyVTKReference_Xor,                // nb_xor
  PyVTKReference_Or,                 // nb_or
  PyVTKReference_Long,               // nb_int
  nullptr,                           // nb_reserved
  PyVTKReference_Float,              // nb_float
  PyVTKReference_InPlaceAdd,         // nb_inplace_add
  PyVTKReference_InPlaceSubtract,    // nb_inplace_subtract
  PyVTKReference_InPlaceMultiply,    // nb_inplace_multiply
  PyVTKReference_InPlaceRemainder,   // nb_inplace_remainder
  PyVTKReference_InPlacePower,       // nb_inplace_power
  PyVTKReference_InPlaceLshift,      // nb_inplace_lshift
  PyVTKReference_InPlaceRshift,      // nb_inplace_rshift
  PyVTKReference_InPlaceAnd,         // nb_inplace_and
  PyVTKReference_InPlaceXor,         // nb_inplace_xor
  PyVTKReference_InPlaceOr,          // nb_inplace_or
  PyVTKReference_FloorDivide,        // nb_floor_divide
  PyVTKReference_TrueDivide,         // nb_true_divide
  PyVTKReference_InPlaceFloorDivide, // nb_inplace_floor_divide
  PyVTKReference_InPlaceTrueDivide,  // nb_inplace_true_divide
  PyVTKReference_Index,              // nb_index
#if PY_VERSION_HEX >= 0x03050000
  nullptr, // nb_matrix_multiply
  nullptr, // nb_inplace_matrix_multiply
#endif
};

//------------------------------------------------------------------------------
static PyNumberMethods PyVTKStringReference_AsNumber = {
  nullptr,                  // nb_add
  nullptr,                  // nb_subtract
  nullptr,                  // nb_multiply
  PyVTKReference_Remainder, // nb_remainder
  nullptr,                  // nb_divmod
  nullptr,                  // nb_power
  nullptr,                  // nb_negative
  nullptr,                  // nb_positive
  nullptr,                  // nb_absolute
  nullptr,                  // nb_nonzero
  nullptr,                  // nb_invert
  nullptr,                  // nb_lshift
  nullptr,                  // nb_rshift
  nullptr,                  // nb_and
  nullptr,                  // nb_xor
  nullptr,                  // nb_or
  nullptr,                  // nb_int
  nullptr,                  // nb_reserved
  nullptr,                  // nb_float
  nullptr,                  // nb_inplace_add
  nullptr,                  // nb_inplace_subtract
  nullptr,                  // nb_inplace_multiply
  nullptr,                  // nb_inplace_remainder
  nullptr,                  // nb_inplace_power
  nullptr,                  // nb_inplace_lshift
  nullptr,                  // nb_inplace_rshift
  nullptr,                  // nb_inplace_and
  nullptr,                  // nb_inplace_xor
  nullptr,                  // nb_inplace_or
  nullptr,                  // nb_floor_divide
  nullptr,                  // nb_true_divide
  nullptr,                  // nb_inplace_floor_divide
  nullptr,                  // nb_inplace_true_divide
  nullptr,                  // nb_index
#if PY_VERSION_HEX >= 0x03050000
  nullptr, // nb_matrix_multiply
  nullptr, // nb_inplace_matrix_multiply
#endif
};
#endif // !Py_LIMITED_API (number method tables)

//------------------------------------------------------------------------------
// Sequence protocol

REFOBJECT_SIZEFUNC(Sequence, Size)
REFOBJECT_BINARYFUNC(Sequence, Concat)
REFOBJECT_INDEXFUNC(Sequence, Repeat)
REFOBJECT_INDEXFUNC(Sequence, GetItem)
REFOBJECT_INTFUNC2(Sequence, Contains)

//------------------------------------------------------------------------------
#if !defined(Py_LIMITED_API)
static PySequenceMethods PyVTKReference_AsSequence = {
  PyVTKReference_Size,     // sq_length
  PyVTKReference_Concat,   // sq_concat
  PyVTKReference_Repeat,   // sq_repeat
  PyVTKReference_GetItem,  // sq_item
  nullptr,                 // sq_slice
  nullptr,                 // sq_ass_item
  nullptr,                 // sq_ass_slice
  PyVTKReference_Contains, // sq_contains
  nullptr,                 // sq_inplace_concat
  nullptr,                 // sq_inplace_repeat
};
#endif

//------------------------------------------------------------------------------
// Mapping protocol

static PyObject* PyVTKReference_GetMapItem(PyObject* ob, PyObject* key)
{
  ob = ((PyVTKReference*)ob)->value;
  return PyObject_GetItem(ob, key);
}

//------------------------------------------------------------------------------
#if !defined(Py_LIMITED_API)
static PyMappingMethods PyVTKReference_AsMapping = {
  PyVTKReference_Size,       // mp_length
  PyVTKReference_GetMapItem, // mp_subscript
  nullptr,                   // mp_ass_subscript
};
#endif

//------------------------------------------------------------------------------
// Buffer protocol

// new buffer protocol
static int PyVTKReference_GetBuffer(PyObject* self, Py_buffer* view, int flags)
{
  PyObject* obj = ((PyVTKReference*)self)->value;
  return PyObject_GetBuffer(obj, view, flags);
}

static void PyVTKReference_ReleaseBuffer(PyObject*, Py_buffer* view)
{
  PyBuffer_Release(view);
}

#if !defined(Py_LIMITED_API)
static PyBufferProcs PyVTKReference_AsBuffer = {
  PyVTKReference_GetBuffer,    // bf_getbuffer
  PyVTKReference_ReleaseBuffer // bf_releasebuffer
};
#endif

//------------------------------------------------------------------------------
// Object protocol

static void PyVTKReference_Delete(PyObject* ob)
{
  Py_DECREF(((PyVTKReference*)ob)->value);
  PyObject_Del(ob);
}

static PyObject* PyVTKReference_Repr(PyObject* ob)
{
  PyObject* r = nullptr;
  const char* name = vtkPythonUtil::GetTypeNameForObject(ob);
  PyObject* s = PyObject_Repr(((PyVTKReference*)ob)->value);
  if (s)
  {
    r = PyUnicode_FromFormat("%s(%U)", name, s);
    Py_DECREF(s);
  }
  return r;
}

static PyObject* PyVTKReference_Str(PyObject* ob)
{
  return PyObject_Str(((PyVTKReference*)ob)->value);
}

static PyObject* PyVTKReference_RichCompare(PyObject* ob1, PyObject* ob2, int opid)
{
  if (PyVTKReference_Check(ob1))
  {
    ob1 = ((PyVTKReference*)ob1)->value;
  }
  if (PyVTKReference_Check(ob2))
  {
    ob2 = ((PyVTKReference*)ob2)->value;
  }
  return PyObject_RichCompare(ob1, ob2, opid);
}

static PyObject* PyVTKReference_GetIter(PyObject* ob)
{
  return PyObject_GetIter(((PyVTKReference*)ob)->value);
}

static PyObject* PyVTKReference_GetAttr(PyObject* self, PyObject* attr)
{
  PyObject* a = PyObject_GenericGetAttr(self, attr);
  if (a || !PyErr_ExceptionMatches(PyExc_AttributeError))
  {
    return a;
  }
  PyErr_Clear();

  int firstchar = '\0';
  if (PyUnicode_GetLength(attr) > 0)
  {
    firstchar = PyUnicode_ReadChar(attr, 0);
  }
  if (firstchar != '_')
  {
    a = PyObject_GetAttr(((PyVTKReference*)self)->value, attr);

    if (a || !PyErr_ExceptionMatches(PyExc_AttributeError))
    {
      return a;
    }
    PyErr_Clear();
  }

  PyErr_Format(PyExc_AttributeError, "'%.50s' object has no attribute '%U'",
    vtkPythonUtil::GetTypeNameForObject(self), attr);
  return nullptr;
}

static PyObject* PyVTKReference_New(PyTypeObject*, PyObject* args, PyObject* kwds)
{
  PyObject* o = nullptr;

  if (kwds && PyDict_Size(kwds))
  {
    PyErr_SetString(PyExc_TypeError, "reference() does not take keyword arguments");
    return nullptr;
  }

  if (PyArg_ParseTuple(args, "O:reference", &o))
  {
    o = PyVTKReference_CompatibleObject(nullptr, o);

    if (o)
    {
      PyVTKReference* self;
      if (PyUnicode_Check(o) || PyBytes_Check(o))
      {
        self = PyObject_New(PyVTKReference, &PyVTKStringReference_Type);
      }
      else if (PyTuple_Check(o) || PyList_Check(o))
      {
        self = PyObject_New(PyVTKReference, &PyVTKTupleReference_Type);
      }
      else
      {
        self = PyObject_New(PyVTKReference, &PyVTKNumberReference_Type);
      }
      self->value = o;
      return (PyObject*)self;
    }
  }

  return nullptr;
}

#ifdef VTK_PYTHON_NEEDS_DEPRECATION_WARNING_SUPPRESSION
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#if !defined(Py_LIMITED_API)
// clang-format off
//------------------------------------------------------------------------------
PyTypeObject PyVTKReference_Type = {
  PyVarObject_HEAD_INIT(&PyType_Type, 0)
  "cvista.vtkCommonCore.reference", // tp_name
  sizeof(PyVTKReference), // tp_basicsize
  0,                      // tp_itemsize
  PyVTKReference_Delete,  // tp_dealloc
#if PY_VERSION_HEX >= 0x03080000
  0,                      // tp_vectorcall_offset
#else
  nullptr,                // tp_print
#endif
  nullptr,                // tp_getattr
  nullptr,                // tp_setattr
  nullptr,                // tp_compare
  PyVTKReference_Repr,    // tp_repr
  nullptr,                // tp_as_number
  nullptr,                // tp_as_sequence
  nullptr,                // tp_as_mapping
  PyObject_HashNotImplemented, // tp_hash
  nullptr,                // tp_call
  PyVTKReference_Str,     // tp_string
  PyVTKReference_GetAttr, // tp_getattro
  nullptr,                // tp_setattro
  nullptr,                // tp_as_buffer
  Py_TPFLAGS_DEFAULT,       // tp_flags
  PyVTKReference_Doc,         // tp_doc
  nullptr,                    // tp_traverse
  nullptr,                    // tp_clear
  PyVTKReference_RichCompare, // tp_richcompare
  0,                          // tp_weaklistoffset
  nullptr,                    // tp_iter
  nullptr,                    // tp_iternext
  PyVTKReference_Methods,     // tp_methods
  nullptr,                    // tp_members
  nullptr,                    // tp_getset
  nullptr,                    // tp_base
  nullptr,                    // tp_dict
  nullptr,                    // tp_descr_get
  nullptr,                    // tp_descr_set
  0,                          // tp_dictoffset
  nullptr,                    // tp_init
  nullptr,                    // tp_alloc
  PyVTKReference_New,         // tp_new
  PyObject_Del,               // tp_free
  nullptr,                    // tp_is_gc
  nullptr,                    // tp_bases
  nullptr,                    // tp_mro
  nullptr,                    // tp_cache
  nullptr,                    // tp_subclasses
  nullptr,                    // tp_weaklist
  VTK_WRAP_PYTHON_SUPPRESS_UNINITIALIZED };

//------------------------------------------------------------------------------
PyTypeObject PyVTKNumberReference_Type = {
  PyVarObject_HEAD_INIT(&PyType_Type, 0)
  "cvista.vtkCommonCore.number_reference", // tp_name
  sizeof(PyVTKReference),   // tp_basicsize
  0,                        // tp_itemsize
  PyVTKReference_Delete,    // tp_dealloc
#if PY_VERSION_HEX >= 0x03080000
  0,                        // tp_vectorcall_offset
#else
  nullptr,                  // tp_print
#endif
  nullptr,                  // tp_getattr
  nullptr,                  // tp_setattr
  nullptr,                  // tp_compare
  PyVTKReference_Repr,      // tp_repr
  &PyVTKReference_AsNumber, // tp_as_number
  nullptr,                  // tp_as_sequence
  nullptr,                  // tp_as_mapping
  PyObject_HashNotImplemented, // tp_hash
  nullptr,                  // tp_call
  PyVTKReference_Str,       // tp_string
  PyVTKReference_GetAttr,   // tp_getattro
  nullptr,                  // tp_setattro
  nullptr,                  // tp_as_buffer
  Py_TPFLAGS_DEFAULT,                // tp_flags
  PyVTKReference_Doc,                  // tp_doc
  nullptr,                             // tp_traverse
  nullptr,                             // tp_clear
  PyVTKReference_RichCompare,          // tp_richcompare
  0,                                   // tp_weaklistoffset
  nullptr,                             // tp_iter
  nullptr,                             // tp_iternext
  PyVTKReference_Methods,              // tp_methods
  nullptr,                             // tp_members
  nullptr,                             // tp_getset
  &PyVTKReference_Type,                // tp_base
  nullptr,                             // tp_dict
  nullptr,                             // tp_descr_get
  nullptr,                             // tp_descr_set
  0,                                   // tp_dictoffset
  nullptr,                             // tp_init
  nullptr,                             // tp_alloc
  PyVTKReference_New,                  // tp_new
  PyObject_Del,                        // tp_free
  nullptr,                             // tp_is_gc
  nullptr,                             // tp_bases
  nullptr,                             // tp_mro
  nullptr,                             // tp_cache
  nullptr,                             // tp_subclasses
  nullptr,                             // tp_weaklist
  VTK_WRAP_PYTHON_SUPPRESS_UNINITIALIZED };

//------------------------------------------------------------------------------
PyTypeObject PyVTKStringReference_Type = {
  PyVarObject_HEAD_INIT(&PyType_Type, 0)
  "cvista.vtkCommonCore.string_reference", // tp_name
  sizeof(PyVTKReference),         // tp_basicsize
  0,                              // tp_itemsize
  PyVTKReference_Delete,          // tp_dealloc
#if PY_VERSION_HEX >= 0x03080000
  0,                              // tp_vectorcall_offset
#else
  nullptr,                        // tp_print
#endif
  nullptr,                        // tp_getattr
  nullptr,                        // tp_setattr
  nullptr,                        // tp_compare
  PyVTKReference_Repr,            // tp_repr
  &PyVTKStringReference_AsNumber, // tp_as_number
  &PyVTKReference_AsSequence,     // tp_as_sequence
  &PyVTKReference_AsMapping,      // tp_as_mapping
  PyObject_HashNotImplemented,    // tp_hash
  nullptr,                        // tp_call
  PyVTKReference_Str,             // tp_string
  PyVTKReference_GetAttr,         // tp_getattro
  nullptr,                        // tp_setattro
  &PyVTKReference_AsBuffer,       // tp_as_buffer
  Py_TPFLAGS_DEFAULT,                // tp_flags
  PyVTKReference_Doc,                  // tp_doc
  nullptr,                             // tp_traverse
  nullptr,                             // tp_clear
  PyVTKReference_RichCompare,          // tp_richcompare
  0,                                   // tp_weaklistoffset
  PyVTKReference_GetIter,              // tp_iter
  nullptr,                             // tp_iternext
  PyVTKReference_Methods,              // tp_methods
  nullptr,                             // tp_members
  nullptr,                             // tp_getset
  &PyVTKReference_Type, // tp_base
  nullptr,                             // tp_dict
  nullptr,                             // tp_descr_get
  nullptr,                             // tp_descr_set
  0,                                   // tp_dictoffset
  nullptr,                             // tp_init
  nullptr,                             // tp_alloc
  PyVTKReference_New,                  // tp_new
  PyObject_Del,                        // tp_free
  nullptr,                             // tp_is_gc
  nullptr,                             // tp_bases
  nullptr,                             // tp_mro
  nullptr,                             // tp_cache
  nullptr,                             // tp_subclasses
  nullptr,                             // tp_weaklist
  VTK_WRAP_PYTHON_SUPPRESS_UNINITIALIZED };

//------------------------------------------------------------------------------
PyTypeObject PyVTKTupleReference_Type = {
  PyVarObject_HEAD_INIT(&PyType_Type, 0)
  "cvista.vtkCommonCore.tuple_reference", // tp_name
  sizeof(PyVTKReference),     // tp_basicsize
  0,                          // tp_itemsize
  PyVTKReference_Delete,      // tp_dealloc
#if PY_VERSION_HEX >= 0x03080000
  0,                          // tp_vectorcall_offset
#else
  nullptr,                    // tp_print
#endif
  nullptr,                    // tp_getattr
  nullptr,                    // tp_setattr
  nullptr,                    // tp_compare
  PyVTKReference_Repr,        // tp_repr
  nullptr,                    // tp_as_number
  &PyVTKReference_AsSequence, // tp_as_sequence
  &PyVTKReference_AsMapping,  // tp_as_mapping
  PyObject_HashNotImplemented, // tp_hash
  nullptr,                    // tp_call
  PyVTKReference_Str,         // tp_string
  PyVTKReference_GetAttr,     // tp_getattro
  nullptr,                    // tp_setattro
  nullptr,                    // tp_as_buffer
  Py_TPFLAGS_DEFAULT,                // tp_flags
  PyVTKReference_Doc,                  // tp_doc
  nullptr,                             // tp_traverse
  nullptr,                             // tp_clear
  PyVTKReference_RichCompare,          // tp_richcompare
  0,                                   // tp_weaklistoffset
  PyVTKReference_GetIter,              // tp_iter
  nullptr,                             // tp_iternext
  PyVTKReference_Methods,              // tp_methods
  nullptr,                             // tp_members
  nullptr,                             // tp_getset
  &PyVTKReference_Type, // tp_base
  nullptr,                             // tp_dict
  nullptr,                             // tp_descr_get
  nullptr,                             // tp_descr_set
  0,                                   // tp_dictoffset
  nullptr,                             // tp_init
  nullptr,                             // tp_alloc
  PyVTKReference_New,                  // tp_new
  PyObject_Del,                        // tp_free
  nullptr,                             // tp_is_gc
  nullptr,                             // tp_bases
  nullptr,                             // tp_mro
  nullptr,                             // tp_cache
  nullptr,                             // tp_subclasses
  nullptr,                             // tp_weaklist
  VTK_WRAP_PYTHON_SUPPRESS_UNINITIALIZED };
// clang-format on
#else // Py_LIMITED_API: heap types via PyType_FromSpec

//------------------------------------------------------------------------------
// abi3 number-protocol slots. The static PyNumberMethods/PySequenceMethods/
// PyMappingMethods/PyBufferProcs tables above cannot exist under the limited
// API (those structs are opaque); the same non-null entries are expressed as
// individual Py_nb_*/Py_sq_*/Py_mp_*/Py_bf_* PyType_Slot rows. Ordering inside
// the spec is immaterial — PyType_FromSpec dispatches by slot id.

// Common (non-number) slots shared by all four reference types.
#define VTK_REF_COMMON_SLOTS                                                                        \
  { Py_tp_dealloc, (void*)PyVTKReference_Delete },                                                  \
  { Py_tp_repr, (void*)PyVTKReference_Repr },                                                       \
  { Py_tp_hash, (void*)PyObject_HashNotImplemented },                                               \
  { Py_tp_str, (void*)PyVTKReference_Str },                                                         \
  { Py_tp_getattro, (void*)PyVTKReference_GetAttr },                                                \
  { Py_tp_doc, (void*)const_cast<char*>(PyVTKReference_Doc) },                                      \
  { Py_tp_richcompare, (void*)PyVTKReference_RichCompare },                                         \
  { Py_tp_methods, (void*)PyVTKReference_Methods },                                                 \
  { Py_tp_new, (void*)PyVTKReference_New },                                                         \
  { Py_tp_free, (void*)PyObject_Del }

// reference (base) — no number/sequence/mapping/buffer.
static PyType_Slot PyVTKReference_Slots[] = {
  VTK_REF_COMMON_SLOTS,
  { 0, nullptr }
};
static PyType_Spec PyVTKReference_Spec = {
  "cvista.vtkCommonCore.reference", sizeof(PyVTKReference), 0,
  // Py_TPFLAGS_BASETYPE is required so number/string/tuple_reference can subclass
  // `reference` via PyType_FromSpecWithBases (the limited API enforces the
  // BASETYPE flag on a base, unlike static types where direct tp_base assignment
  // bypasses the check). Stock's static `reference` is subclassed the same way in
  // C without carrying the flag, so the MRO is identical; only the BASETYPE flag
  // bit appears under abi3 — an intrinsic static->heap artifact in the same class
  // as HEAPTYPE/IMMUTABLETYPE, tracked by the parity gate.
  Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, PyVTKReference_Slots
};

// number_reference — adds the full number protocol; base = reference.
static PyType_Slot PyVTKNumberReference_Slots[] = {
  VTK_REF_COMMON_SLOTS,
  { Py_nb_add, (void*)PyVTKReference_Add },
  { Py_nb_subtract, (void*)PyVTKReference_Subtract },
  { Py_nb_multiply, (void*)PyVTKReference_Multiply },
  { Py_nb_remainder, (void*)PyVTKReference_Remainder },
  { Py_nb_divmod, (void*)PyVTKReference_Divmod },
  { Py_nb_power, (void*)PyVTKReference_Power },
  { Py_nb_negative, (void*)PyVTKReference_Negative },
  { Py_nb_positive, (void*)PyVTKReference_Positive },
  { Py_nb_absolute, (void*)PyVTKReference_Absolute },
  { Py_nb_bool, (void*)PyVTKReference_NonZero },
  { Py_nb_invert, (void*)PyVTKReference_Invert },
  { Py_nb_lshift, (void*)PyVTKReference_Lshift },
  { Py_nb_rshift, (void*)PyVTKReference_Rshift },
  { Py_nb_and, (void*)PyVTKReference_And },
  { Py_nb_xor, (void*)PyVTKReference_Xor },
  { Py_nb_or, (void*)PyVTKReference_Or },
  { Py_nb_int, (void*)PyVTKReference_Long },
  { Py_nb_float, (void*)PyVTKReference_Float },
  { Py_nb_inplace_add, (void*)PyVTKReference_InPlaceAdd },
  { Py_nb_inplace_subtract, (void*)PyVTKReference_InPlaceSubtract },
  { Py_nb_inplace_multiply, (void*)PyVTKReference_InPlaceMultiply },
  { Py_nb_inplace_remainder, (void*)PyVTKReference_InPlaceRemainder },
  { Py_nb_inplace_power, (void*)PyVTKReference_InPlacePower },
  { Py_nb_inplace_lshift, (void*)PyVTKReference_InPlaceLshift },
  { Py_nb_inplace_rshift, (void*)PyVTKReference_InPlaceRshift },
  { Py_nb_inplace_and, (void*)PyVTKReference_InPlaceAnd },
  { Py_nb_inplace_xor, (void*)PyVTKReference_InPlaceXor },
  { Py_nb_inplace_or, (void*)PyVTKReference_InPlaceOr },
  { Py_nb_floor_divide, (void*)PyVTKReference_FloorDivide },
  { Py_nb_true_divide, (void*)PyVTKReference_TrueDivide },
  { Py_nb_inplace_floor_divide, (void*)PyVTKReference_InPlaceFloorDivide },
  { Py_nb_inplace_true_divide, (void*)PyVTKReference_InPlaceTrueDivide },
  { Py_nb_index, (void*)PyVTKReference_Index },
  { 0, nullptr }
};
static PyType_Spec PyVTKNumberReference_Spec = {
  "cvista.vtkCommonCore.number_reference", sizeof(PyVTKReference), 0,
  Py_TPFLAGS_DEFAULT, PyVTKNumberReference_Slots
};

// string_reference — remainder-only number, plus sequence/mapping/buffer/iter.
static PyType_Slot PyVTKStringReference_Slots[] = {
  VTK_REF_COMMON_SLOTS,
  { Py_nb_remainder, (void*)PyVTKReference_Remainder },
  // sequence (PyVTKReference_AsSequence)
  { Py_sq_length, (void*)PyVTKReference_Size },
  { Py_sq_concat, (void*)PyVTKReference_Concat },
  { Py_sq_repeat, (void*)PyVTKReference_Repeat },
  { Py_sq_item, (void*)PyVTKReference_GetItem },
  { Py_sq_contains, (void*)PyVTKReference_Contains },
  // mapping (PyVTKReference_AsMapping)
  { Py_mp_length, (void*)PyVTKReference_Size },
  { Py_mp_subscript, (void*)PyVTKReference_GetMapItem },
  // buffer (PyVTKReference_AsBuffer)
  { Py_bf_getbuffer, (void*)PyVTKReference_GetBuffer },
  { Py_bf_releasebuffer, (void*)PyVTKReference_ReleaseBuffer },
  // iter
  { Py_tp_iter, (void*)PyVTKReference_GetIter },
  { 0, nullptr }
};
static PyType_Spec PyVTKStringReference_Spec = {
  "cvista.vtkCommonCore.string_reference", sizeof(PyVTKReference), 0,
  Py_TPFLAGS_DEFAULT, PyVTKStringReference_Slots
};

// tuple_reference — sequence/mapping/iter, no number/buffer.
static PyType_Slot PyVTKTupleReference_Slots[] = {
  VTK_REF_COMMON_SLOTS,
  { Py_sq_length, (void*)PyVTKReference_Size },
  { Py_sq_concat, (void*)PyVTKReference_Concat },
  { Py_sq_repeat, (void*)PyVTKReference_Repeat },
  { Py_sq_item, (void*)PyVTKReference_GetItem },
  { Py_sq_contains, (void*)PyVTKReference_Contains },
  { Py_mp_length, (void*)PyVTKReference_Size },
  { Py_mp_subscript, (void*)PyVTKReference_GetMapItem },
  { Py_tp_iter, (void*)PyVTKReference_GetIter },
  { 0, nullptr }
};
static PyType_Spec PyVTKTupleReference_Spec = {
  "cvista.vtkCommonCore.tuple_reference", sizeof(PyVTKReference), 0,
  Py_TPFLAGS_DEFAULT, PyVTKTupleReference_Slots
};

// Backing pointers for the `#define PyVTKXxx_Type (*ptr)` shim in the header.
PyTypeObject* PyVTKReference_TypePtr = nullptr;
PyTypeObject* PyVTKNumberReference_TypePtr = nullptr;
PyTypeObject* PyVTKStringReference_TypePtr = nullptr;
PyTypeObject* PyVTKTupleReference_TypePtr = nullptr;

// Build the four heap types, wiring the subclass bases (number/string/tuple
// derive from reference). Idempotent. Returns 0 on success, -1 on failure.
int PyVTKReference_BuildTypes()
{
  if (PyVTKReference_TypePtr)
  {
    return 0;
  }
  PyVTKReference_TypePtr = vtkPythonType_FromSpec(&PyVTKReference_Spec);
  if (!PyVTKReference_TypePtr)
  {
    return -1;
  }
  PyObject* base = (PyObject*)PyVTKReference_TypePtr;
  PyVTKNumberReference_TypePtr =
    (PyTypeObject*)PyType_FromSpecWithBases(&PyVTKNumberReference_Spec, base);
  PyVTKStringReference_TypePtr =
    (PyTypeObject*)PyType_FromSpecWithBases(&PyVTKStringReference_Spec, base);
  PyVTKTupleReference_TypePtr =
    (PyTypeObject*)PyType_FromSpecWithBases(&PyVTKTupleReference_Spec, base);
  if (!PyVTKNumberReference_TypePtr || !PyVTKStringReference_TypePtr ||
    !PyVTKTupleReference_TypePtr)
  {
    return -1;
  }
  return 0;
}
#endif // Py_LIMITED_API
