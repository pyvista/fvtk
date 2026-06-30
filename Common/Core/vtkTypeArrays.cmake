# This file generates arrays specialization subclasses for fixed types,
# like `vtkConstantTypeFloat32Array` or `vtkAffineTypeInt64Array`.
#
# Generated classes are not templated thus they can be wrapped.

include(vtkTypeLists)

# Configure `.in` class files depending on the requested backend
# and the concrete c++ type.
macro(_generate_array_specialization array_prefix vtk_type concrete_type deprecated)
  # used inside .in files
  set(VTK_TYPE_NAME "${vtk_type}")
  set(CONCRETE_TYPE "${concrete_type}")
  if ("${deprecated}")
    set(VTK_DEPRECATION "VTK_DEPRECATED_IN_9_6_0(\"Use vtk${array_prefix}Type*Array instead\")")
  else ()
    set(VTK_DEPRECATION "")
  endif ()

  set(_className "vtk${array_prefix}${VTK_TYPE_NAME}Array")

  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/vtk${array_prefix}TypedArray.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/${_className}.h"
    @ONLY)

  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/vtk${array_prefix}TypedArray.cxx.in"
    "${CMAKE_CURRENT_BINARY_DIR}/${_className}.cxx"
    @ONLY)

  # append generated header to current module headers
  list(APPEND headers
    "${CMAKE_CURRENT_BINARY_DIR}/${_className}.h")

  # append generated source to the bulk instantiation of concrete_type
  if (type MATCHES "^vtkType")
    # String starts with "vtkType"
    vtk_get_fixed_size_type_mapping("${concrete_type}" numeric_type)
    string(REPLACE " " "_" _suffix "${numeric_type}")
  else ()
    string(REPLACE " " "_" _suffix "${concrete_type}")
  endif ()
  if (CVISTA_SPLIT_BULK_INSTANTIATE)
    # cvista split mode: compile each generated specialization (e.g. the
    # vtkType*Array.cxx that define vtkTypeFloat32Array::New() etc.) as its own TU
    # directly, rather than #include-ing it into the per-type bulk TU. Without this
    # these New() definitions would be generated but never compiled -> undefined
    # references at link time. Append straight to `sources` (the list consumed by
    # vtk_module_add_module at the end of CommonCore/CMakeLists.txt): this file is
    # include()d AFTER `set(sources ...)` snapshots `instantiation_sources`, so the
    # bulk path's early-reserved filenames trick is unavailable here; `sources` is
    # still in scope (macros run in the caller scope) and is read later.
    list(APPEND sources
      "${CMAKE_CURRENT_BINARY_DIR}/${_className}.cxx")
    if (CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|AppleClang|Clang)$")
      # VTK_DEPRECATION_LEVEL=0: these implicit-array specialization classes are
      # generated WITH a deprecation attribute (deprecated=1 -> @VTK_DEPRECATION@ =
      # VTK_DEPRECATED_IN_9_6_0(...)) emitted as `class EXPORT <attr> Name : Base`,
      # which GCC < 11 (manylinux2014's GCC 10.2.1) cannot parse. The bulk wrapper
      # suppressed it with a leading `#define VTK_DEPRECATION_LEVEL 0`; the split
      # path compiles these .cxx directly, so it reapplies that define. Byte-
      # identical to the bulk object code (attribute suppressed in both paths).
      set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/${_className}.cxx"
        PROPERTIES COMPILE_OPTIONS "-Wno-attributes"
                   COMPILE_DEFINITIONS "VTK_DEPRECATION_LEVEL=0")
    endif ()
  else ()
    list(APPEND "bulk_instantiation_sources_${_suffix}"
      "#include \"${_className}.cxx\"")
  endif ()

  unset(VTK_DEPRECATION)
  unset(VTK_TYPE_NAME)
  unset(CONCRETE_TYPE)
  unset(_className)
endmacro()

# VTK_DEPRECATED_IN_9_6_0 to be removed later
foreach (array_prefix IN ITEMS Affine Composite Constant Indexed)
  foreach (type IN LISTS vtk_numeric_types)
    vtk_type_to_camel_case("${type}" cased_type)
    _generate_array_specialization("${array_prefix}" "${cased_type}" "${type}" 1)
  endforeach ()
endforeach ()

# cvista: StdFunction + Strided are the two dead families (CVISTA_DROP_DEAD_ARRAYS,
# default ON) — omit their fixed-size specialization classes from the generated
# set. The keep set always includes the load-bearing AOS/SOA/ScaledSOA + the
# implicit Affine/Composite/Constant/Indexed families (used by PyVista's
# ImageData/structured grids and the dispatcher).
set(_cvista_specialization_prefixes Affine Composite Constant Indexed ScaledSOA SOA)
if (NOT CVISTA_DROP_DEAD_ARRAYS)
  list(APPEND _cvista_specialization_prefixes StdFunction Strided)
endif ()
foreach (array_prefix IN LISTS _cvista_specialization_prefixes)
  foreach (type IN LISTS vtk_fixed_size_numeric_types)
    vtk_fixed_size_type_to_without_prefix("${type}" "vtk" without_vtk_prefix)
    _generate_array_specialization("${array_prefix}" "${without_vtk_prefix}" "${type}" 0)
  endforeach ()
endforeach ()

function(vtk_type_native type ctype class)
  string(TOUPPER "${type}" type_upper)
  set("vtk_type_native_${type}" "
#if VTK_TYPE_${type_upper} == VTK_${ctype}
# include \"${class}Array.h\"
# define vtkTypeArrayBase ${class}Array
#endif
"
    PARENT_SCOPE)
endfunction()

function(vtk_type_native_choice type preferred_ctype preferred_class fallback_ctype fallback_class)
  string(TOUPPER "${type}" type_upper)
  set("vtk_type_native_${type}" "
#if VTK_TYPE_${type_upper} == VTK_${preferred_ctype}
# include \"${preferred_class}Array.h\"
# define vtkTypeArrayBase ${preferred_class}Array
#elif VTK_TYPE_${type_upper} == VTK_${fallback_ctype}
# include \"${fallback_class}Array.h\"
# define vtkTypeArrayBase ${fallback_class}Array
#endif
"
    PARENT_SCOPE)
endfunction()

# Configure data arrays for platform-independent fixed-size types.
# Match the type selection here to that in vtkType.h.
vtk_type_native(Int8 SIGNED_CHAR vtkSignedChar)
vtk_type_native(UInt8 UNSIGNED_CHAR vtkUnsignedChar)
vtk_type_native(Int16 SHORT vtkShort)
vtk_type_native(UInt16 UNSIGNED_SHORT vtkUnsignedShort)
vtk_type_native(Int32 INT vtkInt)
vtk_type_native(UInt32 UNSIGNED_INT vtkUnsignedInt)
vtk_type_native_choice(Int64 LONG vtkLong LONG_LONG vtkLongLong)
vtk_type_native_choice(UInt64 UNSIGNED_LONG vtkUnsignedLong UNSIGNED_LONG_LONG vtkUnsignedLongLong)
vtk_type_native(Float32 FLOAT vtkFloat)
vtk_type_native(Float64 DOUBLE vtkDouble)

foreach (type IN LISTS vtk_fixed_size_numeric_types)
  vtk_fixed_size_type_to_without_prefix("${type}" "vtkType" vtk_type)
  set(VTK_TYPE_NAME "${vtk_type}")
  set(VTK_TYPE_NATIVE "${vtk_type_native_${vtk_type}}")
  if (VTK_TYPE_NATIVE)
    configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/vtkAOSTypedArray.h.in"
      "${CMAKE_CURRENT_BINARY_DIR}/${type}Array.h"
      @ONLY)
    configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/vtkAOSTypedArray.cxx.in"
      "${CMAKE_CURRENT_BINARY_DIR}/${type}Array.cxx"
      @ONLY)
    # append generated header to current module headers
    list(APPEND headers
      "${CMAKE_CURRENT_BINARY_DIR}/${type}Array.h")
    # append generated source to the bulk instantiation of concrete_type
    vtk_get_fixed_size_type_mapping("${type}" numeric_type)
    string(REPLACE " " "_" _suffix "${numeric_type}")
    if (CVISTA_SPLIT_BULK_INSTANTIATE)
      # cvista split mode: compile the plain vtkType*Array.cxx (vtkTypeFloat64Array
      # etc., which define their New()/ctor) as its own TU instead of #include-ing
      # it into the per-type bulk TU. See the matching branch in the macro above;
      # append to `sources` (read by vtk_module_add_module later) since this runs
      # after `set(sources ...)` snapshotted `instantiation_sources`.
      list(APPEND sources
        "${CMAKE_CURRENT_BINARY_DIR}/${type}Array.cxx")
      if (CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|AppleClang|Clang)$")
        # VTK_DEPRECATION_LEVEL=0 to match the bulk wrapper's leading #define (see
        # the macro above + vtkArrayBulkInstantiate.cxx.in). Keeps split TUs
        # byte-identical to bulk and parseable on GCC < 11.
        set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/${type}Array.cxx"
          PROPERTIES COMPILE_OPTIONS "-Wno-attributes"
                     COMPILE_DEFINITIONS "VTK_DEPRECATION_LEVEL=0")
      endif ()
    else ()
      list(APPEND "bulk_instantiation_sources_${_suffix}"
        "#include \"${type}Array.cxx\"")
    endif ()
  endif ()
endforeach ()
