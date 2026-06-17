set(classes
  PyVTKEnum
  PyVTKExtras
  PyVTKNamespace
  PyVTKObject
  PyVTKSpecialObject
  PyVTKTemplate
  vtkPythonArgs
  vtkPythonCommand
  vtkPythonOverload
  vtkPythonUtil
  vtkSmartPyObject)

set(private_classes
  PyVTKMethodDescriptor
  PyVTKReference)

set(headers
  vtkPythonCompatibility.h)

set(private_headers
  PyVTKExtras.h)

get_property(vtk_python_version_major GLOBAL
  PROPERTY _vtk_python_version_major)
get_property(vtk_python_version_minor GLOBAL
  PROPERTY _vtk_python_version_minor)

vtk_module_add_module(VTK::WrappingPythonCore
  CLASSES ${classes}
  HEADERS ${headers}
  PRIVATE_CLASSES ${private_classes}
  PRIVATE_HEADERS ${private_headers}
  LIBRARY_NAME_SUFFIX "${vtk_python_version_major}.${vtk_python_version_minor}")

# fvtk: Python stable-ABI (abi3) — EXPERIMENTAL. See docs/abi3-feasibility.md.
# Compile the wrapper runtime itself against the CPython limited API so it can
# be linked into a single cp3x-abi3 wheel. Currently a diagnostic lever: this
# runtime is not yet limited-API clean, so the build surfaces the porting
# worklist rather than producing a working library.
if (FVTK_ABI3)
  vtk_module_definitions(VTK::WrappingPythonCore
    PRIVATE
      "Py_LIMITED_API=${FVTK_ABI3_VERSION}")
endif ()

if (NOT VTK_ABI_NAMESPACE_NAME STREQUAL "<DEFAULT>" AND NOT DEFINED ENV{CI})
  message(WARNING "Wrappings::PythonCore ABI does not not support the VTK_ABI_NAMESPACE_NAME "
                  "and the symbols will not be mangled.")
endif ()

if (Python3_VERSION VERSION_LESS "3.7" AND NOT CMAKE_VERSION VERSION_LESS "3.19")
  include(CheckCompilerFlag)
  set(old_python_ignore_warning_flag "-Wno-writable-strings")
  check_compiler_flag(CXX "${old_python_ignore_warning_flag}" "vtk_have_compiler_flag-CXX-${old_python_ignore_warning_flag}")
  if (vtk_have_compiler_flag-CXX-${old_python_ignore_warning_flag})
    vtk_module_compile_options(VTK::WrappingPythonCore
      PRIVATE
        "${old_python_ignore_warning_flag}")
  endif ()
endif ()
