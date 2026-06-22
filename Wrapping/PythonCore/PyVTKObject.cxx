// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
/*-----------------------------------------------------------------------
  The PyVTKObject was created in Oct 2000 by David Gobbi for VTK 3.2.
  Support for weakref added in July 2005 by Prabhu Ramachandran.
  Buffer interface for vtk arrays added in Feb 2008 by Berk Geveci.

  A PyVTKObject is a python object that represents a VTK object.
  The methods are stored in the __dict__ of the associated type objects.
  Each PyVTKObject also has a __dict__ of its own that can be used to
  store arbitrary attributes.

  Memory management is done as follows. Each PyVTKObject has
  an entry along with a smart pointer to its vtkObjectBase in
  the vtkPythonUtil::ObjectMap.  When a PyVTKObject is destructed,
  it is removed along with the smart pointer from the ObjectMap.
-----------------------------------------------------------------------*/

#include "PyVTKObject.h"
#include "PyVTKMethodDescriptor.h"
#include "vtkABINamespace.h"
#include "vtkDataArray.h"
#include "vtkObjectBase.h"
#include "vtkPythonCommand.h"
#include "vtkPythonTypeAccess.h"
#include "vtkPythonUtil.h"
#include "vtkStringFormatter.h"

#include <cstddef>
#include <dictobject.h>
#include <sstream>

// This will be set to the python type struct for vtkObjectBase
static PyTypeObject* PyVTKObject_Type = nullptr;

VTK_ABI_NAMESPACE_BEGIN
//------------------------------------------------------------------------------
PyVTKClass::PyVTKClass(
  PyTypeObject* typeobj, PyMethodDef* methods, const char* classname, vtknewfunc constructor)
{
  this->py_type = typeobj;
  this->py_methods = methods;
  this->vtk_name = classname;
  this->vtk_new = constructor;
}
VTK_ABI_NAMESPACE_END

//------------------------------------------------------------------------------
// Create a Python "override" method
// See the help string below this function for details.
static PyObject* PyVTKClass_override(PyObject* cls, PyObject* type)
{
  PyTypeObject* typeobj = (PyTypeObject*)cls;
  std::string clsName = vtkPythonUtil::StripModuleFromType(typeobj);

  if (PyObject_TypeCheck(type, &PyType_Type))
  {
    PyTypeObject* newtypeobj = (PyTypeObject*)type;
    if (PyType_IsSubtype(newtypeobj, typeobj))
    {
      // Make sure "type" and intermediate classes aren't wrapped classes
      for (PyTypeObject* tp = newtypeobj; tp && tp != typeobj; tp = vtkPythonType_GetBase(tp))
      {
        PyVTKClass* c = vtkPythonUtil::FindClass(vtkPythonUtil::StripModuleFromType(tp));
        if (c && tp == c->py_type)
        {
          std::string str("method requires overriding with a pure python subclass of ");
          str += clsName;
          str += ", subclassing from VTK C++ subclasses is not allowed.";
          PyErr_SetString(PyExc_TypeError, str.c_str());
          return nullptr;
        }
      }

      // Set the override
      PyVTKClass* thecls = vtkPythonUtil::FindClass(clsName.c_str());
      thecls->py_type = newtypeobj;
      // Store override in dict of old type, to keep a reference to it
      vtkPythonType_SetDictItem(typeobj, "__override__", type);
    }
    else
    {
      std::string str("method requires a subtype of ");
      str += clsName;
      PyErr_SetString(PyExc_TypeError, str.c_str());
      return nullptr;
    }
  }
  else if (type == Py_None)
  {
    // Clear the override
    PyVTKClass* thecls = vtkPythonUtil::FindClass(clsName.c_str());
    thecls->py_type = typeobj;
    // Delete the __override__ attribute if it exists
    if (vtkPythonType_DelDictItem(typeobj, "__override__") == -1)
    {
      // Clear the KeyError that occurs if __override__ doesn't exist
      PyErr_Clear();
    }
  }
  else
  {
    PyErr_SetString(PyExc_TypeError, "method requires a type object or None.");
    return nullptr;
  }

  Py_INCREF(type);
  return type;
}

static PyMethodDef PyVTKClass_override_def = { "override", PyVTKClass_override, METH_CLASS | METH_O,
  "This method can be used to override a VTK class with a Python subclass.\n"
  "The class type passed to override will afterwards be instantiated\n"
  "instead of the type override is called on.\n"
  "For example,\n"
  "\n"
  "class foo(vtk.vtkPoints):\n"
  "  pass\n"
  "vtk.vtkPoints.override(foo)\n"
  "\n"
  "will lead to foo being instantied every time vtkPoints() is called.\n"
  "The main objective of this functionality is to enable developers to\n"
  "extend VTK classes with more pythonic subclasses that contain\n"
  "convenience functionality.\n" };

//------------------------------------------------------------------------------
// Add a class, add methods and members to its type object.  A return
// value of nullptr signifies that the class was already added.
//
// Under the default build the type object is a statically-defined PyTypeObject
// that the generator passes in by address; this routine lazily creates its
// tp_dict and populates the method descriptors. Under Py_LIMITED_API the type
// cannot be a static object (PyTypeObject is opaque), so the generator instead
// passes the PyType_Spec and the resolved base; the abi3 overload below builds
// the heap type with PyType_FromSpec and routes every dict insertion through the
// SetDictItem accessor (heap types own their dict and the slot is unwritable).
// Both paths register the same (classname -> PyTypeObject*) map entry and emit
// observably identical __vtkname__ / method / override descriptors.
#if defined(Py_LIMITED_API)
PyTypeObject* PyVTKClass_Add(PyType_Spec* spec, PyTypeObject* base, PyMethodDef* methods,
  const char* classname, vtknewfunc constructor)
{
  // Idempotency: if this class is already registered, hand back its type.
  if (PyVTKClass* existing = vtkPythonUtil::FindClass(classname))
  {
    return existing->py_type;
  }

  // Build the heap type from the generated spec. A base resolved at runtime
  // (cross-module, or a sibling class) is supplied as the single base; when
  // none is given (vtkObjectBase) PyType_FromSpec defaults the base to object.
  PyObject* pyobj;
  if (base != nullptr)
  {
    PyObject* bases = PyTuple_Pack(1, reinterpret_cast<PyObject*>(base));
    pyobj = PyType_FromSpecWithBases(spec, bases);
    Py_XDECREF(bases);
  }
  else
  {
    pyobj = PyType_FromSpec(spec);
  }
  if (pyobj == nullptr)
  {
    // PyType_FromSpec failed (e.g. a bad base); the exception is already set and
    // is propagated to the importing interpreter.
    return nullptr;
  }
  PyTypeObject* pytype = reinterpret_cast<PyTypeObject*>(pyobj);

  // Register in the class map (keyed on classname). This stores the type; the
  // returned pointer is what every caller uses thereafter. The map holds the
  // reference for the life of the interpreter (matching the static "lives
  // forever" semantics of the default build).
  pytype = vtkPythonUtil::AddClassToMap(pytype, methods, classname, constructor);

  // Cache the type object for vtkObjectBase for quick access
  if (PyVTKObject_Type == nullptr && strcmp(classname, "vtkObjectBase") == 0)
  {
    PyVTKObject_Type = pytype;
  }

  // Add special attribute __vtkname__
  PyObject* s = PyUnicode_FromString(classname);
  vtkPythonType_SetDictItem(pytype, "__vtkname__", s);
  Py_DECREF(s);

  // Add all of the methods
  for (PyMethodDef* meth = methods; meth && meth->ml_name; meth++)
  {
    PyObject* func = PyVTKMethodDescriptor_New(pytype, meth);
    vtkPythonType_SetDictItem(pytype, meth->ml_name, func);
    Py_DECREF(func);
  }

  // Add the override method
  if (strcmp(classname, "vtkObjectBase") == 0)
  {
    PyObject* func = PyDescr_NewClassMethod(pytype, &PyVTKClass_override_def);
    vtkPythonType_SetDictItem(pytype, PyVTKClass_override_def.ml_name, func);
    Py_DECREF(func);
  }
  return pytype;
}
#else
PyTypeObject* PyVTKClass_Add(
  PyTypeObject* pytype, PyMethodDef* methods, const char* classname, vtknewfunc constructor)
{
  // Check whether the type is already in the map (use classname as key),
  // and return it if so.  If not, then add it to the map.
  pytype = vtkPythonUtil::AddClassToMap(pytype, methods, classname, constructor);

  // Cache the type object for vtkObjectBase for quick access
  if (PyVTKObject_Type == nullptr && strcmp(classname, "vtkObjectBase") == 0)
  {
    PyVTKObject_Type = pytype;
  }

  // If type object already has a dict, we're done
  if (pytype->tp_dict)
  {
    return pytype;
  }

  // Create the dict
  pytype->tp_dict = PyDict_New();

  // Add special attribute __vtkname__
  PyObject* s = PyUnicode_FromString(classname);
  PyDict_SetItemString(pytype->tp_dict, "__vtkname__", s);
  Py_DECREF(s);

  // Add all of the methods
  for (PyMethodDef* meth = methods; meth && meth->ml_name; meth++)
  {
    PyObject* func = PyVTKMethodDescriptor_New(pytype, meth);
    PyDict_SetItemString(pytype->tp_dict, meth->ml_name, func);
    Py_DECREF(func);
  }

  // Add the override method
  if (strcmp(classname, "vtkObjectBase") == 0)
  {
    PyObject* func = PyDescr_NewClassMethod(pytype, &PyVTKClass_override_def);
    PyDict_SetItemString(pytype->tp_dict, PyVTKClass_override_def.ml_name, func);
    Py_DECREF(func);
  }
  return pytype;
}
#endif

void PyVTKClass_AddCombinedGetSetDefinitions(PyTypeObject* pytype, PyGetSetDef* getsets)
{
#if defined(Py_LIMITED_API)
  // Defensive under abi3: a NULL pytype means an upstream PyType_FromSpec failed
  // (exception already set); skip rather than dereference it.
  if (pytype == nullptr)
  {
    return;
  }
#endif
  // Add all of the getsets
  for (PyGetSetDef* getset = getsets; getset && getset->name; getset++)
  {
    if (getset->get == nullptr)
    {
      // find a getter in superclass
      if (vtkPythonType_GetBase(pytype) != nullptr)
      {
        auto key = PyUnicode_FromString(getset->name);
        if (auto superGetSet =
              vtkPythonUtil::FindGetSetDescriptor(vtkPythonType_GetBase(pytype), key))
        {
          getset->get = superGetSet->get;
          if (getset->closure)
          {
            static_cast<PyVTKGetSet*>(getset->closure)->get =
              static_cast<PyVTKGetSet*>(superGetSet->closure)->get;
          }
        }
        Py_DECREF(key);
      }
    }
    else if (getset->set == nullptr)
    {
      // find a setter in superclass
      if (vtkPythonType_GetBase(pytype) != nullptr)
      {
        auto key = PyUnicode_FromString(getset->name);
        if (auto superGetSet =
              vtkPythonUtil::FindGetSetDescriptor(vtkPythonType_GetBase(pytype), key))
        {
          getset->set = superGetSet->set;
          if (getset->closure)
          {
            static_cast<PyVTKGetSet*>(getset->closure)->set =
              static_cast<PyVTKGetSet*>(superGetSet->closure)->set;
          }
        }
        Py_DECREF(key);
      }
    }
    PyObject* descr = PyDescr_NewGetSet(pytype, getset);
    vtkPythonType_SetDictItem(pytype, getset->name, descr);
#if defined(Py_LIMITED_API)
    // Record the backing PyGetSetDef so FindGetSetDescriptor can recover it under
    // the limited API (the descriptor's d_getset field is then unreadable).
    vtkPythonUtil::RegisterGetSetDescriptor(pytype, getset->name, getset);
#endif
    Py_DECREF(descr);
  }
}

//------------------------------------------------------------------------------
int PyVTKObject_Check(PyObject* op)
{
  return PyObject_TypeCheck(op, PyVTKObject_Type);
}

//------------------------------------------------------------------------------
// Object protocol

//------------------------------------------------------------------------------
PyObject* PyVTKObject_String(PyObject* op)
{
  std::ostringstream vtkmsg_with_warning_C4701;
  ((PyVTKObject*)op)->vtk_ptr->Print(vtkmsg_with_warning_C4701);
  vtkmsg_with_warning_C4701.put('\0');
  PyObject* res = PyUnicode_FromString(vtkmsg_with_warning_C4701.str().c_str());
  return res;
}

//------------------------------------------------------------------------------
PyObject* PyVTKObject_Repr(PyObject* op)
{
  PyVTKObject* obj = (PyVTKObject*)op;
  return PyUnicode_FromFormat("<%s(%p) at %p>", vtkPythonUtil::GetTypeNameForObject(op),
    static_cast<void*>(obj->vtk_ptr), static_cast<void*>(obj));
}

//------------------------------------------------------------------------------
int PyVTKObject_Traverse(PyObject* o, visitproc visit, void* arg)
{
  PyVTKObject* self = (PyVTKObject*)o;
  int err = 0;

  if (self->vtk_observers != nullptr)
  {
    unsigned long* olist = self->vtk_observers;
    while (err == 0 && *olist != 0)
    {
      vtkObject* op = static_cast<vtkObject*>(self->vtk_ptr);
      vtkCommand* c = op->GetCommand(*olist);
      if (c == nullptr)
      {
        // observer is gone, remove from list
        unsigned long* tmp = olist;
        do
        {
          tmp++;
        } while (*tmp != 0);
        *olist = *--tmp;
        *tmp = 0;
      }
      else
      {
        // visit the observer
        vtkPythonCommand* cbc = static_cast<vtkPythonCommand*>(c);
        err = visit(cbc->obj, arg);
        olist++;
      }
    }
  }

  return err;
}

//------------------------------------------------------------------------------
PyObject* PyVTKObject_New(PyTypeObject* tp, PyObject* args, PyObject* /*kwds*/)
{
  // XXX(python3-abi3): all types will be heap types in abi3
  // If type was subclassed within python, then skip arg checks and
  // simply create a new object.
  PyObject* o = nullptr;
  if ((PyType_GetFlags(tp) & Py_TPFLAGS_HEAPTYPE) == 0)
  {
    if (!PyArg_UnpackTuple(args, vtkPythonUtil::GetTypeName(tp), 0, 1, &o))
    {
      return nullptr;
    }

    if (o)
    {
      // used to create a VTK object from a SWIG pointer
      return vtkPythonUtil::GetObjectFromObject(o, vtkPythonUtil::StripModuleFromType(tp));
    }
  }

  // if PyVTKObject_FromPointer gets nullptr, it creates a new object.
  o = PyVTKObject_FromPointer(tp, nullptr, nullptr);

  return o;
}

//------------------------------------------------------------------------------
int PyVTKObject_Init(PyObject* obj, PyObject* /*args*/, PyObject* kwds)
{
  bool success = true;
  if (kwds != nullptr && PyDict_Size(kwds))
  {
    PyObject *key, *value;
    Py_ssize_t ppos = 0;
    // Walks through every keyword argument and sets the property on the object.
    // Loosely equivalent to this python code with error handling omitted.
    // for key, value in kwargs.items():
    //     setattr(obj, key, value)
    PyTypeObject* tp = Py_TYPE(obj);
    while (PyDict_Next(kwds, &ppos, &key, &value) && success)
    {
      // Check if the key is an existing property.
      if (vtkPythonUtil::FindGetSetDescriptor(tp, key) != nullptr)
      {
        // If the setter failed, break.
        if (PyObject_SetAttr(obj, key, value) == -1)
        {
          // Don't try to raise an exception here because it's already raised by PyObject_SetAttr.
          success = false;
        }
      }
      else
      {
        PyObject* utf8StringObj = PyUnicode_AsUTF8String(key);
        std::string keyName = utf8StringObj ? PyBytes_AsString(utf8StringObj) : "(null)";
        Py_XDECREF(utf8StringObj);
        // Raise TypeError for unexpected keyword argument.
        PyErr_SetString(PyExc_TypeError,
          ("Unexpected keyword argument \'" + keyName + "\' for \'" +
            vtkPythonUtil::GetTypeName(tp) + "\' constructor")
            .c_str());
        success = false;
      }
    }
  }
  return success ? 0 : -1;
}

//------------------------------------------------------------------------------
void PyVTKObject_Delete(PyObject* op)
{
  PyVTKObject* self = (PyVTKObject*)op;

  PyObject_GC_UnTrack(op);

  if (self->vtk_weakreflist != nullptr)
  {
    PyObject_ClearWeakRefs(op);
  }

  // A python object owning a VTK object reference is getting
  // destroyed.  Remove the python object's VTK object reference.
  vtkPythonUtil::RemoveObjectFromMap(op);

  Py_DECREF(self->vtk_dict);
  delete[] self->vtk_observers;
  delete[] self->vtk_buffer;

  PyObject_GC_Del(op);
}

//------------------------------------------------------------------------------
// Generic property getter

PyObject* PyVTKObject_GetProperty(PyObject* op, void* methods)
{
  // getter takes no arguments (empty arg tuple)
  PyObject* args = PyTuple_New(0);
  PyObject* result = static_cast<PyVTKGetSet*>(methods)->get(op, args);
  Py_DECREF(args);
  return result;
}

//------------------------------------------------------------------------------
// Generic property setter

int PyVTKObject_SetProperty(PyObject* op, PyObject* value, void* methods)
{
  PyObject* args = PyTuple_Pack(1, value);
  PyObject* result = static_cast<PyVTKGetSet*>(methods)->set(op, args);
  Py_DECREF(args);
  if (result == nullptr)
  {
    return -1;
  }
  Py_DECREF(result);
  return 0;
}

//------------------------------------------------------------------------------
// Setter that splits the value into multiple arguments for the set method

int PyVTKObject_SetPropertyMulti(PyObject* op, PyObject* value, void* methods)
{
  PyObject* args;
  if (PySequence_Check(value) && !PyUnicode_Check(value))
  {
    // if value is sequence, apply its members as arguments
    args = PySequence_Tuple(value);
  }
  else
  {
    // try passing the value as a single argument
    args = PyTuple_Pack(1, value);
  }
  PyObject* result = static_cast<PyVTKGetSet*>(methods)->set(op, args);
  Py_DECREF(args);
  if (result == nullptr)
  {
    return -1;
  }
  Py_DECREF(result);
  return 0;
}

//------------------------------------------------------------------------------
// This defines any special attributes of wrapped VTK objects.

static PyObject* PyVTKObject_GetDict(PyObject* op, void*)
{
  PyVTKObject* self = (PyVTKObject*)op;
  Py_INCREF(self->vtk_dict);
  return self->vtk_dict;
}

static PyObject* PyVTKObject_GetThis(PyObject* op, void*)
{
  PyVTKObject* self = (PyVTKObject*)op;
  const char* classname = self->vtk_ptr->GetClassName();
  const char* cp = classname;
  char buf[1024];
  // check to see if classname is a valid python identifier
  if (isalpha(*cp) || *cp == '_')
  {
    do
    {
      cp++;
    } while (isalnum(*cp) || *cp == '_');
  }
  // otherwise, use the pythonic form of the class name
  if (*cp != '\0')
  {
    classname = vtkPythonUtil::StripModuleFromObject(op);
  }
  auto result = vtk::format_to_n(buf, sizeof(buf), "p_{:.500s}", classname);
  *result.out = '\0';
  return PyUnicode_FromString(vtkPythonUtil::ManglePointer(self->vtk_ptr, buf));
}

#if PY_VERSION_HEX >= 0x03070000
#define pystr(x) x
#else
#define pystr(x) const_cast<char*>(x)
#endif

PyGetSetDef PyVTKObject_GetSet[] = {
#if !defined(Py_LIMITED_API)
  // Default build: every wrapped class's static type carries the custom "__dict__"
  // getset. Under abi3 a heap subclass may not re-declare an inherited "__dict__"
  // descriptor (PyType_FromSpec rejects it), so "__dict__" is carried ONLY by
  // vtkObjectBase's spec getset (PyVTKObject_BaseGetSet) and inherited downward.
  { pystr("__dict__"), PyVTKObject_GetDict, nullptr,
    pystr("Dictionary of attributes set by user."), nullptr },
#endif
  { pystr("__this__"), PyVTKObject_GetThis, nullptr, pystr("Pointer to the C++ object."), nullptr },
  { nullptr, nullptr, nullptr, nullptr, nullptr } };

#if defined(Py_LIMITED_API)
// abi3: vtkObjectBase's spec uses this getset (adds "__dict__"); subclasses use
// PyVTKObject_GetSet (no "__dict__") and inherit the descriptor through the MRO.
PyGetSetDef PyVTKObject_BaseGetSet[] = { { pystr("__dict__"), PyVTKObject_GetDict, nullptr,
                                           pystr("Dictionary of attributes set by user."), nullptr },
  { pystr("__this__"), PyVTKObject_GetThis, nullptr, pystr("Pointer to the C++ object."), nullptr },
  { nullptr, nullptr, nullptr, nullptr, nullptr } };
#endif

//------------------------------------------------------------------------------
// The following methods and struct define the "buffer" protocol
// for PyVTKObject, so that python can read from a vtkDataArray.
// This is particularly useful for NumPy.

//------------------------------------------------------------------------------
// Convert a VTK type to a python type char (struct module)
static const char* pythonTypeFormat(int t)
{
  const char* b = nullptr;

  switch (t)
  {
    case VTK_CHAR:
      b = "c";
      break;
    case VTK_SIGNED_CHAR:
      b = "b";
      break;
    case VTK_UNSIGNED_CHAR:
      b = "B";
      break;
    case VTK_SHORT:
      b = "h";
      break;
    case VTK_UNSIGNED_SHORT:
      b = "H";
      break;
    case VTK_INT:
      b = "i";
      break;
    case VTK_UNSIGNED_INT:
      b = "I";
      break;
    case VTK_LONG:
      b = "l";
      break;
    case VTK_UNSIGNED_LONG:
      b = "L";
      break;
    case VTK_LONG_LONG:
      b = "q";
      break;
    case VTK_UNSIGNED_LONG_LONG:
      b = "Q";
      break;
    case VTK_FLOAT:
      b = "f";
      break;
    case VTK_DOUBLE:
      b = "d";
      break;
#ifndef VTK_USE_64BIT_IDS
    case VTK_ID_TYPE:
      b = "i";
      break;
#else
    case VTK_ID_TYPE:
      b = "q";
      break;
#endif
  }

  return b;
}

//------------------------------------------------------------------------------
// Under abi3 these have external linkage so the generator's PyType_Spec can wire
// them into the Py_bf_getbuffer / Py_bf_releasebuffer slots (the limited API has
// no PyBufferProcs struct). Under the default build they stay file-static and
// are attached via the static PyBufferProcs table below, byte-for-byte as before.
#if defined(Py_LIMITED_API)
int PyVTKObject_AsBuffer_GetBuffer(PyObject* obj, Py_buffer* view, int flags)
#else
static int PyVTKObject_AsBuffer_GetBuffer(PyObject* obj, Py_buffer* view, int flags)
#endif
{
  PyVTKObject* self = (PyVTKObject*)obj;
  vtkDataArray* da = vtkDataArray::SafeDownCast(self->vtk_ptr);
  if (da)
  {
    void* ptr = da->GetVoidPointer(0);
    Py_ssize_t ntuples = da->GetNumberOfTuples();
    int ncomp = da->GetNumberOfComponents();
    int dsize = da->GetDataTypeSize();
    const char* format = pythonTypeFormat(da->GetDataType());
    Py_ssize_t size = ntuples * ncomp * dsize;

    if (da->GetDataType() == VTK_BIT)
    {
      size = (ntuples * ncomp + 7) / 8;
    }

    // start by building a basic "unsigned char" buffer
    if (PyBuffer_FillInfo(view, obj, ptr, size, 0, flags) == -1)
    {
      return -1;
    }
    // check if a dimensioned array was requested
    if (format != nullptr && (flags & PyBUF_ND) != 0)
    {
      // first, build a simple 1D array
      view->itemsize = dsize;
      view->ndim = (ncomp > 1 ? 2 : 1);
      view->format = const_cast<char*>(format);

      {
        if (self->vtk_buffer && self->vtk_buffer[0] != view->ndim)
        {
          delete[] self->vtk_buffer;
          self->vtk_buffer = nullptr;
        }
        if (self->vtk_buffer == nullptr)
        {
          self->vtk_buffer = new Py_ssize_t[2 * view->ndim + 1];
          self->vtk_buffer[0] = view->ndim;
        }
        view->shape = &self->vtk_buffer[1];
        view->strides = &self->vtk_buffer[view->ndim + 1];
      }

      if (view->ndim == 1)
      {
        // simple one-dimensional array
        view->shape[0] = ntuples * ncomp;
        view->strides[0] = view->itemsize;
      }
      else
      {
        // use native C dimension ordering by default
        char order = 'C';
        if ((flags & PyBUF_ANY_CONTIGUOUS) == PyBUF_F_CONTIGUOUS)
        {
          // use fortran ordering only if explicitly requested
          order = 'F';
        }
        // need to allocate space for the strides and shape
        view->shape[0] = ntuples;
        view->shape[1] = ncomp;
        if (order == 'F')
        {
          view->shape[0] = ncomp;
          view->shape[1] = ntuples;
        }
        PyBuffer_FillContiguousStrides(view->ndim, view->shape, view->strides, dsize, order);
      }
    }
    return 0;
  }

  PyErr_Format(
    PyExc_ValueError, "Cannot get a buffer from %s.", vtkPythonUtil::GetTypeNameForObject(obj));
  return -1;
}

//------------------------------------------------------------------------------
#if defined(Py_LIMITED_API)
void PyVTKObject_AsBuffer_ReleaseBuffer(PyObject* obj, Py_buffer* view)
#else
static void PyVTKObject_AsBuffer_ReleaseBuffer(PyObject* obj, Py_buffer* view)
#endif
{
  // nothing to do, the caller will decref the obj
  (void)obj;
  (void)view;
}

//------------------------------------------------------------------------------
#if !defined(Py_LIMITED_API)
PyBufferProcs PyVTKObject_AsBuffer = {
  PyVTKObject_AsBuffer_GetBuffer,    // bf_getbuffer
  PyVTKObject_AsBuffer_ReleaseBuffer // bf_releasebuffer
};
#endif

//------------------------------------------------------------------------------
PyObject* PyVTKObject_FromPointer(PyTypeObject* pytype, PyObject* ghostdict, vtkObjectBase* ptr)
{
  // This will be set if we create a new C++ object
  bool created = false;
  std::string classname = vtkPythonUtil::StripModuleFromType(pytype);
  PyVTKClass* cls = nullptr;

#if defined(Py_LIMITED_API)
  // Under abi3 every wrapped type is a heap type, so the Py_TPFLAGS_HEAPTYPE test
  // further down cannot tell a Python-defined subclass from a wrapped VTK class.
  // A wrapped VTK class is registered in the class map under its (module-stripped)
  // name; a Python-defined subclass is not. Record that here from the originally-
  // requested name (before `classname` is reassigned below) so factory New() /
  // actual-class retyping still applies to wrapped classes -- e.g. vtkSkybox()
  // returns vtkOpenGLSkybox, matching stock VTK -- while Python subclasses keep
  // their own type.
  const bool requestedIsWrappedClass = (vtkPythonUtil::FindClass(classname.c_str()) != nullptr);
#endif

  if (ptr)
  {
    // If constructing from an existing C++ object, use its actual class
    classname = ptr->GetClassName();
    cls = vtkPythonUtil::FindClass(classname.c_str());
  }

  if (cls == nullptr)
  {
    // Use the vtkname of the supplied class type
    PyObject* s = PyObject_GetAttrString((PyObject*)pytype, "__vtkname__");
    if (s)
    {
      PyObject* tmp = PyUnicode_AsUTF8String(s);
      if (tmp)
      {
        Py_DECREF(s);
        s = tmp;
      }
      const char* vtkname_classname = PyBytes_AsString(s);
      if (vtkname_classname == nullptr)
      {
        Py_DECREF(s);
        return nullptr;
      }
      classname = vtkname_classname;
      Py_DECREF(s);
    }
    cls = vtkPythonUtil::FindClass(classname.c_str());
    if (cls == nullptr)
    {
      PyErr_Format(PyExc_ValueError, "internal error, unknown VTK class %.200s", classname.c_str());
      return nullptr;
    }
  }

  if (!ptr)
  {
    // Create a new instance of this class since we were not given one.
    if (cls->vtk_new)
    {
      ptr = cls->vtk_new();
      if (!ptr)
      {
        // The vtk_new() method returns null when a factory class has no
        // implementation (i.e. cannot provide a concrete class instance.)
        // NotImplementedError indicates a pure virtual method call.
        PyErr_SetString(
          PyExc_NotImplementedError, "no concrete implementation exists for this class");
        return nullptr;
      }

      // Check if the VTK object already has a Python object
      // (e.g. vtk_new() might return a singleton instance)
      PyObject* obj = vtkPythonUtil::FindObject(ptr);
      if (obj)
      {
        ptr->Delete();
        return obj;
      }

      // flag to indicate that the VTK object is a new instance
      created = true;

      // Check the type of the newly-created object
      const char* newclassname = ptr->GetClassName();
      if (std::string(newclassname) != classname)
      {
        PyVTKClass* newclass = vtkPythonUtil::FindClass(newclassname);
        if (newclass)
        {
          classname = newclassname;
          cls = newclass;
        }
      }
    }
    else
    {
      PyErr_SetString(PyExc_TypeError, "this is an abstract class and cannot be instantiated");
      return nullptr;
    }
  }

#if defined(Py_LIMITED_API)
  // All wrapped types are heap types under abi3, so the static-type branch below
  // (pytype = cls->py_type, which honors factory New() / the object's actual
  // class) would never run. Apply it here for genuine wrapped VTK classes --
  // identified as the canonical type for their own name -- so that vtkSkybox(),
  // vtkActor(), vtkPolyDataMapper(), ... return their vtkOpenGL* factory override
  // exactly as stock VTK does. Python-defined subclasses (pytype !=
  // requestedType) keep their own type. Heap types are refcounted and
  // PyObject_GC_New borrows a reference, so incref the final type either way.
  if (requestedIsWrappedClass && cls->py_type != nullptr)
  {
    pytype = cls->py_type;
  }
  Py_INCREF(reinterpret_cast<PyObject*>(pytype));
#else
  if ((PyType_GetFlags(pytype) & Py_TPFLAGS_HEAPTYPE) != 0)
  {
    // Incref if class was declared in python (see PyType_GenericAlloc).
    Py_INCREF(reinterpret_cast<PyObject*>(pytype));
  }
  else
  {
    // To support factory New methods, use the object's actual class
    pytype = cls->py_type;
  }
#endif

  // Create a new dict unless object is being resurrected from a ghost
  PyObject* pydict = ghostdict;
  if (pydict)
  {
    Py_INCREF(pydict);
  }
  else
  {
    pydict = PyDict_New();
  }

  PyVTKObject* self = PyObject_GC_New(PyVTKObject, pytype);

  self->vtk_ptr = ptr;
  self->vtk_flags = 0;
  self->vtk_class = cls;
  self->vtk_dict = pydict;
  self->vtk_buffer = nullptr;
  self->vtk_observers = nullptr;
  self->vtk_weakreflist = nullptr;

  PyObject_GC_Track((PyObject*)self);

  // A python object owning a VTK object reference is getting
  // created.  Add the python object's VTK object reference.
  vtkPythonUtil::AddObjectToMap((PyObject*)self, ptr);

  // The hash now owns a reference so we can free ours.
  if (created)
  {
    ptr->Delete();
  }
  else if (ghostdict == nullptr && vtkPythonType_GetInit(pytype) != nullptr)
  {
    // For checking if Python __init__ call modifies the C++ object
    vtkObject* checkptr = vtkObject::SafeDownCast(ptr);
    vtkMTimeType checktime = 0;
    if (checkptr)
    {
      checktime = checkptr->vtkObject::GetMTime();
    }
    // Call __init__(self)
    PyObject* arglist = Py_BuildValue("()");
    int res = vtkPythonType_GetInit(pytype)((PyObject*)self, arglist, nullptr);
    Py_DECREF(arglist);
    if (res < 0)
    {
      Py_DECREF(self);
      self = nullptr;
    }
    else if (checkptr && checktime < checkptr->vtkObject::GetMTime())
    {
      // If the C++ object already existed within VTK, and the Python object
      // is being created right now, then we don't want the creation of the
      // Python object to cause changes to object attributes that were already
      // set by the C++ VTK code.  For example, if we're getting the output
      // dataset from a C++ VTK filter, we don't want any C++ attributes of
      // the dataset to change when the Python part of the dataset object is
      // created and initialized.
      std::string message = "Python method ";
      message += vtkPythonUtil::GetTypeName(pytype);
      message += ".__init__() ";
      message += "unexpectedly modified pre-existing C++ base object ";
      message += checkptr->GetObjectDescription();
      message += ".";
      PyErr_WarnEx(PyExc_RuntimeWarning, message.c_str(), 1);
    }
  }

  return (PyObject*)self;
}

vtkObjectBase* PyVTKObject_GetObject(PyObject* obj)
{
  return ((PyVTKObject*)obj)->vtk_ptr;
}

void PyVTKObject_AddObserver(PyObject* obj, unsigned long id)
{
  unsigned long* olist = ((PyVTKObject*)obj)->vtk_observers;
  unsigned long n = 0;
  if (olist == nullptr)
  {
    olist = new unsigned long[8];
    ((PyVTKObject*)obj)->vtk_observers = olist;
  }
  else
  {
    // count the number of items
    while (olist[n] != 0)
    {
      n++;
    }
    // check if n+1 is a power of two (base allocation is 8)
    unsigned long m = n + 1;
    if (m >= 8 && (n & m) == 0)
    {
      unsigned long* tmp = olist;
      olist = new unsigned long[2 * m];
      for (unsigned long i = 0; i < n; i++)
      {
        olist[i] = tmp[i];
      }
      delete[] tmp;
      ((PyVTKObject*)obj)->vtk_observers = olist;
    }
  }
  olist[n++] = id;
  olist[n] = 0;
}

unsigned int PyVTKObject_GetFlags(PyObject* obj)
{
  return ((PyVTKObject*)obj)->vtk_flags;
}

void PyVTKObject_SetFlag(PyObject* obj, unsigned int flag, int val)
{
  if (val)
  {
    ((PyVTKObject*)obj)->vtk_flags |= flag;
  }
  else
  {
    ((PyVTKObject*)obj)->vtk_flags &= ~flag;
  }
}
