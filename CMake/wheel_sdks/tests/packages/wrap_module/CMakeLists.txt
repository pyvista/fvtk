cmake_minimum_required(VERSION 3.15...4.0)

project(${SKBUILD_PROJECT_NAME} VERSION ${SKBUILD_PROJECT_VERSION})

# Python and VTK are both found through scikit-build-core's CMake prefix-path
# injections: VTK comes from the build-time `fvtk-sdk` dependency (its
# `cmake.prefix` entry point), Python from the active interpreter.
#
# WrappingPythonCore is the hand-written Python<->C++ runtime that wrapped
# modules link against; requesting it as a component proves the SDK exposes both
# that module and `vtkModuleWrapPython.cmake` (the config includes the latter
# whenever VTK_WRAP_PYTHON is on), so `vtk_module_wrap_python` below is defined.
find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
find_package(VTK REQUIRED COMPONENTS
  CommonCore
  CommonDataModel
  FiltersCore
  WrappingPythonCore)

# Build a shared module (matches the SDK's own shared VTK libs) and co-locate the
# module lib with the wrapped extension inside the `sdk_example` package so the
# installed wheel is self-contained, with the module lib resolvable next to the
# extension via an origin-relative rpath.
#
# scikit-build-core sets an absolute CMAKE_INSTALL_PREFIX and absolute
# GNUInstallDirs; vtkModule.cmake rejects absolute *_DESTINATION values, and
# vtk_module_build's destinations all derive from these four dirs. Set them
# RELATIVE so every derived destination (ARCHIVE/RUNTIME/HEADERS/CMAKE/LICENSE/
# SPDX/HIERARCHY) is relative and passes the guard.
set(BUILD_SHARED_LIBS ON)
set(CMAKE_INSTALL_LIBDIR "sdk_example")
set(CMAKE_INSTALL_BINDIR "sdk_example")
set(CMAKE_INSTALL_INCLUDEDIR "include")
set(CMAKE_INSTALL_DATAROOTDIR "share")
if (APPLE)
  set(CMAKE_INSTALL_RPATH "@loader_path")
elseif (UNIX)
  set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif ()

# Scan + build the out-of-tree module declared in sdk_example/vtk.module.
vtk_module_scan(
  MODULE_FILES      "${CMAKE_CURRENT_SOURCE_DIR}/sdk_example/vtk.module"
  REQUEST_MODULES   SDKExample
  PROVIDES_MODULES  sdk_example_modules
  ENABLE_TESTS      OFF)

vtk_module_build(
  MODULES         ${sdk_example_modules}
  INSTALL_HEADERS OFF)

# Python-wrap the module into the `sdk_example` package. SOABI tags the wrapped
# library with the interpreter's extension suffix; the module destination is the
# package directory so the wrapped extension lands beside its __init__.py.
vtk_module_wrap_python(
  MODULES             ${sdk_example_modules}
  INSTALL_HEADERS     OFF
  BUILD_PYI_FILES     OFF
  PYTHON_PACKAGE      "sdk_example"
  MODULE_DESTINATION  "."
  SOABI               "${Python_SOABI}"
  WRAPPED_MODULES     sdk_example_wrapped_modules)

# scikit-build-core packages whatever is installed under CMAKE_INSTALL_PREFIX;
# add the package marker so `sdk_example` is an importable package.
install(
  FILES       "${CMAKE_CURRENT_SOURCE_DIR}/sdk_example/__init__.py"
  DESTINATION "sdk_example")
