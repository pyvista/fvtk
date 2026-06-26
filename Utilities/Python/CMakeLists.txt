#[==[.md
# Python support

This module provides a single target for using Python within VTK modules.

## Flexible Python libraries

Ideally, the libraries linked against the `vtkPython` module would be flexible
in the library actually used and it would be provided by the executable loading
the Python instead. This can be made to work with hacks currently, but the
following should be fixed first:

  - A better solution than `-undefined dynamic_lookup` for macOS. VTK has
    [an issue][VTK dynamic Python lookup issue] filed for this already.

[VTK dynamic Python lookup issue]: https://gitlab.kitware.com/vtk/vtk/-/issues/17214
#]==]

# VTK only supports a single Python version at a time, so make artifact finding
# interactive.
set(Python3_ARTIFACTS_INTERACTIVE ON)

set(vtk_python_min_version "3.4")
set(vtk_python_version_support "3.9")
if (VTK_REMOVE_LEGACY)
  set(vtk_python_min_version "${vtk_python_version_support}")
endif ()
vtk_module_find_package(
  PACKAGE Python3
  VERSION "${vtk_python_min_version}"
  COMPONENTS Interpreter Development.Module
  OPTIONAL_COMPONENTS Development.Embed
  FORWARD_VERSION_REQ MINOR)
set(vtk_python_includes "${Python3_INCLUDE_DIRS}")
set(vtk_python_target "Python3::Module")
set(vtk_python_embed_target "Python3::Python")
set(vtk_python_version "${Python3_VERSION}")
set(vtk_python_version_pair "${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}")

# cvista abi3 (Py_LIMITED_API) Windows link fix — cross-version dependency (PART 2).
#
# Under Py_LIMITED_API (the abi3 default), MSVC's pyconfig.h emits
#   #pragma comment(lib, "python3.lib")
# so every TU that includes <Python.h> auto-links the stable-ABI forwarding
# import lib python3.lib (-> python3.dll). FindPython, however, hands
# Python3::Module an IMPORTED_IMPLIB of the version-specific python3XX.lib
# (absolute path), so every Python-using DLL/.pyd would also gain a python3XX.dll
# dependency and fail to import on a different CPython minor (cibuildwheel tests
# the single abi3 wheel on cp312/cp313/cp314).
#
# PART 1 (resolving the bare python3.lib the #pragma auto-links -> LNK1104) is
# handled in the backend via the MSVC `LIB` env var (see
# ci/cibw/cvista_backend.py::_set_abi3_windows_libdir) — NOT via /LIBPATH in the
# linker flags, because ThirdParty/hdf5 embeds those flags into a C string
# literal that a quoted Windows path corrupts.
#
# PART 2, below: retarget Python3::Module's import lib to the stable-ABI stub
# python3.lib so the abi3 wheel depends on python3.dll, not python3XX.dll. The
# detection loop locates python3.lib's directory; the retarget sticks across all
# subdir scopes because CMAKE_FIND_PACKAGE_TARGETS_GLOBAL (set in minimal.cmake,
# MSVC only) makes Python3::Module a single global target. WIN32 + abi3 only;
# Linux/macOS (.abi3.so, no #pragma comment(lib), no version import lib) untouched.
if (WIN32 AND CVISTA_ABI3)
  set(_cvista_abi3_py_libdirs)
  # The python312.lib that FindPython links lives in the same dir as python3.lib;
  # prefer the actual library path's directory, then FindPython's reported dirs,
  # then the standard CPython <prefix>/include -> <prefix>/libs sibling fallback.
  foreach (_cvista_abi3_pyvar IN ITEMS
      Python3_SABI_LIBRARY Python3_LIBRARY Python3_LIBRARY_RELEASE)
    if (${_cvista_abi3_pyvar} AND EXISTS "${${_cvista_abi3_pyvar}}")
      get_filename_component(_cvista_abi3_d "${${_cvista_abi3_pyvar}}" DIRECTORY)
      if (_cvista_abi3_d)
        list(APPEND _cvista_abi3_py_libdirs "${_cvista_abi3_d}")
      endif ()
    endif ()
  endforeach ()
  foreach (_cvista_abi3_pylib IN LISTS Python3_LIBRARIES Python3_SABI_LIBRARIES)
    if (EXISTS "${_cvista_abi3_pylib}")
      get_filename_component(_cvista_abi3_d "${_cvista_abi3_pylib}" DIRECTORY)
      if (_cvista_abi3_d)
        list(APPEND _cvista_abi3_py_libdirs "${_cvista_abi3_d}")
      endif ()
    endif ()
  endforeach ()
  foreach (_cvista_abi3_dirvar IN ITEMS
      Python3_SABI_LIBRARY_DIRS Python3_LIBRARY_DIRS Python3_RUNTIME_LIBRARY_DIRS)
    if (${_cvista_abi3_dirvar})
      list(APPEND _cvista_abi3_py_libdirs ${${_cvista_abi3_dirvar}})
    endif ()
  endforeach ()
  foreach (_cvista_abi3_inc IN LISTS Python3_INCLUDE_DIRS)
    get_filename_component(_cvista_abi3_prefix "${_cvista_abi3_inc}" DIRECTORY)
    if (_cvista_abi3_prefix AND IS_DIRECTORY "${_cvista_abi3_prefix}/libs")
      list(APPEND _cvista_abi3_py_libdirs "${_cvista_abi3_prefix}/libs")
    endif ()
  endforeach ()
  if (_cvista_abi3_py_libdirs)
    list(REMOVE_DUPLICATES _cvista_abi3_py_libdirs)

    # abi3 portability: make every Python-using DLL/.pyd depend on python3.dll,
    # NOT python3XX.dll. FindPython hands Python3::Module an IMPORTED_IMPLIB of
    # the version-specific python3XX.lib (absolute path), so every consumer would
    # otherwise gain a python312.dll dependency and fail to import on cp313/cp314
    # (cibuildwheel tests the single abi3 wheel on cp312/cp313/cp314). Retarget
    # the imported lib to the stable-ABI forwarding stub python3.lib (same libs/
    # dir, exports the same limited-API symbols, forwards to python3.dll). One
    # override covers WrappingPythonCore AND every wrapper .pyd through the normal
    # VTK::Python -> Python3::Module link, with no /NODEFAULTLIB or absolute-path
    # lib hacks. /NODEFAULTLIB:python3XX.lib would NOT work — FindPython passes
    # python3XX.lib by absolute path, which /NODEFAULTLIB (bare-name only) ignores.
    find_file(_cvista_abi3_python3_lib
      NAMES python3.lib
      PATHS ${_cvista_abi3_py_libdirs}
      NO_DEFAULT_PATH)
    if (_cvista_abi3_python3_lib AND TARGET Python3::Module)
      get_target_property(_cvista_abi3_mod_type Python3::Module TYPE)
      # Only retarget the import-library form (shared CPython -> .lib stub). A
      # static/embedded libpython has no python3.lib and must keep its real lib.
      get_target_property(_cvista_abi3_mod_implib Python3::Module IMPORTED_IMPLIB)
      if (_cvista_abi3_mod_implib)
        set_target_properties(Python3::Module PROPERTIES
          IMPORTED_IMPLIB "${_cvista_abi3_python3_lib}")
        # Cover per-config implibs too, if FindPython set them.
        foreach (_cvista_abi3_cfg RELEASE DEBUG RELWITHDEBINFO MINSIZEREL)
          get_target_property(_cvista_abi3_cfg_implib
            Python3::Module IMPORTED_IMPLIB_${_cvista_abi3_cfg})
          if (_cvista_abi3_cfg_implib)
            set_target_properties(Python3::Module PROPERTIES
              IMPORTED_IMPLIB_${_cvista_abi3_cfg} "${_cvista_abi3_python3_lib}")
          endif ()
        endforeach ()
        message(STATUS
          "cvista abi3: Python3::Module import lib retargeted to ${_cvista_abi3_python3_lib} "
          "(abi3 wheel depends on python3.dll, not python3${Python3_VERSION_MINOR}.dll)")
      endif ()
    else ()
      message(WARNING
        "cvista abi3: python3.lib not found in ${_cvista_abi3_py_libdirs}; the abi3 "
        "wheel may keep a version-specific python3XX.dll dependency and fail to "
        "import on other CPython minors.")
    endif ()
  else ()
    message(WARNING
      "cvista abi3: could not determine the CPython libs/ directory; python3.lib "
      "may fail to link under Py_LIMITED_API on Windows (LNK1104).")
  endif ()
endif ()

set_property(GLOBAL PROPERTY _vtk_python_soabi "${Python3_SOABI}")
set_property(GLOBAL PROPERTY _vtk_python_version_major "${Python3_VERSION_MAJOR}")
set_property(GLOBAL PROPERTY _vtk_python_version_minor "${Python3_VERSION_MINOR}")

# Check deprecated versions of Python
if (NOT VTK_LEGACY_SILENT AND vtk_python_version VERSION_LESS vtk_python_version_support)
  message(DEPRECATION
    "Python ${vtk_python_version} support is deprecated, use Python ${vtk_python_version_support}+")
endif ()

# Export location of python module dirs in install and build tree for every vtkpython module to use
# As long as those modules depend on vtkpython, they can retrieve and use these
if (NOT VTK_PYTHON_SITE_PACKAGES_SUFFIX)
  if (MSVC)
    set(VTK_PYTHON_SITE_PACKAGES_SUFFIX "Lib/site-packages")
  else ()
    set(VTK_PYTHON_SITE_PACKAGES_SUFFIX
      "python${vtk_python_version_pair}/site-packages")
  endif ()
endif ()

if (CMAKE_CONFIGURATION_TYPES)
  # For build systems with configuration types e.g. Xcode/Visual Studio,
  # we rely on generator expressions.
  set(VTK_BUILD_PYTHON_MODULES_DIR
    "${CMAKE_BINARY_DIR}/$<CONFIG>/${VTK_PYTHON_SITE_PACKAGES_SUFFIX}")
else ()
  set(VTK_BUILD_PYTHON_MODULES_DIR
    "${CMAKE_BINARY_DIR}/${VTK_PYTHON_SITE_PACKAGES_SUFFIX}")
endif ()

if (NOT DEFINED VTK_INSTALL_PYTHON_MODULES_DIR)
  if (WIN32 AND NOT CYGWIN)
    set(VTK_INSTALL_PYTHON_MODULES_DIR
      "${VTK_PYTHON_SITE_PACKAGES_SUFFIX}")
  else ()
    set(VTK_INSTALL_PYTHON_MODULES_DIR
      "${VTK_PYTHON_SITE_PACKAGES_SUFFIX}")
  endif ()
endif ()

# For static builds, since vtkPythonInterpreter cannot work off the library
# location, but instead has to use the executable location, it needs to know the
# library dir explicitly.
if (NOT BUILD_SHARED_LIBS)
  set(VTK_PYTHON_SITE_PACKAGES_SUFFIX_FIXED "${VTK_INSTALL_PYTHON_MODULES_DIR}")
else ()
  set(VTK_PYTHON_SITE_PACKAGES_SUFFIX_FIXED "${VTK_PYTHON_SITE_PACKAGES_SUFFIX}")
endif ()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/vtkPythonConfigure.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkPythonConfigure.h")

set(headers
  vtkPython.h
  "${CMAKE_CURRENT_BINARY_DIR}/vtkPythonConfigure.h")

vtk_module_add_module(VTK::Python
  HEADERS ${headers}
  HEADER_ONLY)

include(CMakeDependentOption)
cmake_dependent_option(VTK_PYTHON_OPTIONAL_LINK
  # Default to `ON` if it is possible.
  "Whether to link libpython from libraries or not" ON
  # We shouldn't do it for static builds and we can't do it without
  # `target_link_options`. Windows also always needs to link against libpython
  # directly.
  "NOT WIN32;BUILD_SHARED_LIBS;COMMAND target_link_options" OFF)

add_library(PythonUsed INTERFACE)
add_library(VTK::PythonUsed ALIAS PythonUsed)
_vtk_module_install(PythonUsed)

# If we want optional linking and we have a real libpython, set up the forward
# linking.
if (VTK_PYTHON_OPTIONAL_LINK AND TARGET "${vtk_python_embed_target}")
  # It has been observed that Ubuntu's GCC toolchain defaults require flags,
  # but are not detected here due to the confluence of the flags (basically,
  # the errors only show up in an executable that links to Python-using code,
  # but does not use Python itself either). Since this is not detected, setting
  # `vtk_undefined_symbols_allowed=OFF` manually may be required when using
  # Ubuntu toolchains.
  include("${CMAKE_CURRENT_SOURCE_DIR}/vtkUndefinedSymbolsAllowed.cmake")
  set(is_exe "$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>")
  set(needs_flags "$<NOT:$<BOOL:${vtk_undefined_symbols_allowed}>>")
  set(direct_link "$<BOOL:$<TARGET_PROPERTY:INTERFACE_vtk_python_direct_link>>")
  set(should_use "$<OR:${is_exe},${direct_link}>")
  # Intel's Fortran compiler doesn't like some flags, so mask it out when using
  # the Intel Fortran Compiler (OneAPI).
  if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.15") # XXX(cmake-3.15): `COMPILE_LANG_AND_ID` genex
    set(ifx_mask "$<NOT:$<COMPILE_LANG_AND_ID:Fortran,IntelLLVM>>")
  else ()
    set(ifx_mask 1)
  endif ()
  set(platform_flags
    # Apple flags.
    "$<$<PLATFORM_ID:Darwin>:-undefined;dynamic_lookup>"
    # Linux flags.
    # The linker is allowed to skip libraries not used by the end result. This
    # can exclude the `libpython` DT_NEEDED entry at the final executable if it
    # itself does not actually use any libpython symbols.
    "$<$<AND:${should_use},$<PLATFORM_ID:Linux>>:LINKER:--no-as-needed>"
    # Due to the above making the symbols actually available, we can ignore
    # unresolved symbols in the shared libraries that are being linked to.
    "$<$<AND:${should_use},$<PLATFORM_ID:Linux>,${ifx_mask}>:LINKER:--unresolved-symbols=ignore-in-shared-libs>")

  # Use the `PythonX::Module` target from FindPython.
  vtk_module_link(VTK::Python
    INTERFACE
      "${vtk_python_target}")

  # This target is intended for use by the end-result executable with a
  # `VTK::Python` link in its total library closure. This exposes the
  # `PythonX::Python` link at the executable level with the required platform
  # flags.
  target_link_libraries(PythonUsed
    INTERFACE
      "$<LINK_ONLY:$<${should_use}:${vtk_python_embed_target}>>")
  target_link_options(PythonUsed
    INTERFACE
      "$<$<AND:${should_use},${needs_flags}>:${platform_flags}>")
  _vtk_module_set_module_property(VTK::Python APPEND
    PROPERTY  "forward_link"
    VALUE     VTK::PythonUsed)
# Otherwise, if we have the libpython and aren't doing optional linking, just
# use libpython.
elseif (NOT VTK_PYTHON_OPTIONAL_LINK AND TARGET "${vtk_python_embed_target}")
  vtk_module_link(VTK::Python
    INTERFACE
      "${vtk_python_embed_target}")
# Do the optional linking support.
else ()
  vtk_module_link(VTK::Python
    INTERFACE
      "${vtk_python_target}")
endif ()
