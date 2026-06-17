include(CMakeDependentOption)
cmake_dependent_option(VTK_USE_64BIT_TIMESTAMPS "Build VTK with 64 bit timestamps" OFF
  "NOT CMAKE_SIZEOF_VOID_P GREATER 4" ON)
mark_as_advanced(VTK_USE_64BIT_TIMESTAMPS)

# Option to mangle VTK symbols
set(VTK_ABI_NAMESPACE_NAME
  "<DEFAULT>"
  CACHE STRING "Inline namespace name for mangling VTK symbols.")
mark_as_advanced(VTK_ABI_NAMESPACE_NAME)
set(VTK_ABI_NAMESPACE_ATTRIBUTES
  "<DEFAULT>"
  CACHE STRING "Inline namespace attributes for mangling VTK symbols.")
mark_as_advanced(VTK_ABI_NAMESPACE_ATTRIBUTES)

if (VTK_ABI_NAMESPACE_NAME STREQUAL "<DEFAULT>")
  # The default is to not have an ABI namespace.
  # This is consistent with the existing practice.
  set(VTK_ABI_NAMESPACE_NAME "")
endif ()
set_property(GLOBAL PROPERTY _vtk_abi_namespace_name "${VTK_ABI_NAMESPACE_NAME}")

if (VTK_ABI_NAMESPACE_ATTRIBUTES STREQUAL "<DEFAULT>")
  set(VTK_ABI_NAMESPACE_ATTRIBUTES "")
endif()
set_property(GLOBAL PROPERTY _vtk_abi_namespace_attributes "${VTK_ABI_NAMESPACE_ATTRIBUTES}")

if (VTK_ABI_NAMESPACE_NAME STREQUAL "")
  set(VTK_HAS_ABI_NAMESPACE OFF)
  set(VTK_ABI_NAMESPACE_BEGIN "")
  set(VTK_ABI_NAMESPACE_END "")
else ()
  message(STATUS "VTK ABI namespace: ${VTK_ABI_NAMESPACE_NAME}")
  set(_vtk_abi_namespace_name_regex "^[a-zA-Z0-9_]+$")
  if (NOT VTK_ABI_NAMESPACE_NAME MATCHES "${_vtk_abi_namespace_name_regex}")
    message(FATAL_ERROR
      "The configured VTK_ABI_NAMESPACE_NAME='${VTK_ABI_NAMESPACE_NAME}' is "
      "not supported, it must match the regular expression "
      "'${_vtk_abi_namespace_name_regex}'.  To add things like attributes, "
      "configure VTK with -DVTK_ABI_NAMESPACE_ATTRIBUTES='"
      "__attribute__((visibility(\"hidden\")))'.")
  endif()
  unset(_vtk_abi_namespace_name_regex)
  set(VTK_HAS_ABI_NAMESPACE ON)
  set(VTK_ABI_NAMESPACE_BEGIN "inline namespace ${VTK_ABI_NAMESPACE_ATTRIBUTES} ${VTK_ABI_NAMESPACE_NAME} {")
  set(VTK_ABI_NAMESPACE_END "}")
endif ()

if (VTK_ABI_NAMESPACE_ATTRIBUTES STREQUAL "")
  set(VTK_HAS_ABI_NAMESPACE_ATTRIBUTES OFF)
else ()
  set(VTK_HAS_ABI_NAMESPACE_ATTRIBUTES ON)
endif ()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/vtkABINamespace.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkABINamespace.h"
  @ONLY)

if (CMAKE_SIZEOF_VOID_P GREATER 4)
 set(vtk_use_64bit_ids_default ON)
else ()
 set(vtk_use_64bit_ids_default OFF)
endif ()

option(VTK_USE_64BIT_IDS "Build VTK with 64 bit ids" "${vtk_use_64bit_ids_default}")
mark_as_advanced(VTK_USE_64BIT_IDS)

option(VTK_USE_FUTURE_CONST "Adds more const to APIs. Opt-in because it may break compatibility" OFF)
mark_as_advanced(VTK_USE_FUTURE_CONST)

option(VTK_USE_FUTURE_BOOL "Changes some ints to bools. Opt-in because it may break compatibility" OFF)
mark_as_advanced(VTK_USE_FUTURE_BOOL)

option(VTK_DEBUG_LEAKS "Build leak checking support into VTK." OFF)
mark_as_advanced(VTK_DEBUG_LEAKS)

# See vtkDataArrayRange.h docs for more info on these:
cmake_dependent_option(VTK_DEBUG_RANGE_ITERATORS
  "Detect usage errors with the for-range iterators (SLOW)." OFF
  "CMAKE_BUILD_TYPE STREQUAL \"Debug\"" OFF)
mark_as_advanced(VTK_DEBUG_RANGE_ITERATORS)
cmake_dependent_option(VTK_ALWAYS_OPTIMIZE_ARRAY_ITERATORS
  "Enable additional optimizations for array iterators in Debug builds." OFF
  "NOT VTK_DEBUG_RANGE_ITERATORS" OFF)
mark_as_advanced(VTK_ALWAYS_OPTIMIZE_ARRAY_ITERATORS)

# This option determines the behavior of the New methods of vtkObject derived
# classes. If this option is off then only classes specifically using the
# vtkObjectFactoryNewMacro and vtkAbstractObjectFactoryNewMacro will allow for
# overrides. If the method is on then vtkStandardNewMacro will use the
# vtkObjectFactoryNewMacro's implementation body.
option(VTK_ALL_NEW_OBJECT_FACTORY
  "Build all vtkObject derived classes with object factory new methods." OFF)
mark_as_advanced(VTK_ALL_NEW_OBJECT_FACTORY)

set(vtk_smp_sources)
set(vtk_smp_headers)
set(vtk_smp_nowrap_headers)
set(vtk_smp_templates)
set(vtk_smp_defines)
set(vtk_smp_libraries)
set(vtk_smp_enable_sequential 0)
set(vtk_smp_enable_stdthread 0)
set(vtk_smp_enable_tbb 0)
set(vtk_smp_enable_openmp 0)
set(VTK_SMP_IMPLEMENTATION_TYPE)
include("${CMAKE_CURRENT_SOURCE_DIR}/vtkSMPSelection.cmake")

set(vtk_object_base_defines)

# An optional dependency on memkind
set(vtk_memkind_libraries)
if (VTK_USE_MEMKIND)
  vtk_module_find_package(
    PACKAGE MEMKIND)
  list(APPEND vtk_memkind_libraries
    MEMKIND::MEMKIND)
  # XXX(memkind-1.10): Once 1.10 is required, this is always true.
  file(READ "${MEMKIND_INCLUDE_DIR}/memkind.h" memkind_h_contents)
  set(memkind_has_dax_kmem 0)
  # Look for the symbol in the header.
  if (memkind_h_contents MATCHES " MEMKIND_DAX_KMEM[ ;\n]")
    set(memkind_has_dax_kmem 1)
  endif ()
  list(APPEND vtk_object_base_defines "VTK_MEMKIND_HAS_DAX_KMEM=$<BOOL:${memkind_has_dax_kmem}>")
endif ()

# Generate the vtkTypeList_Create macros:
include("${CMAKE_CURRENT_SOURCE_DIR}/vtkCreateTypeListMacros.cmake")
CreateTypeListMacros(
  VTK_TYPELISTMACRO_HEADER_CONTENTS
  99
  vtkTypeList_Create
  "vtkTypeList::TypeList"
  "vtkTypeList::NullType")
configure_file(vtkTypeListMacros.h.in vtkTypeListMacros.h)

# And the ArrayDispatch array list header:
option(VTK_DISPATCH_AOS_ARRAYS "Include array-of-structs vtkDataArray subclasses in dispatcher." ON)
option(VTK_DISPATCH_SOA_ARRAYS "Include struct-of-arrays vtkDataArray subclasses in dispatcher." OFF)
option(VTK_DISPATCH_SCALED_SOA_ARRAYS "Include scaled struct-of-arrays vtkDataArray subclasses in dispatcher." OFF)

option(VTK_DISPATCH_AFFINE_ARRAYS "Include implicit vtkDataArray subclasses based on an affine function backend in dispatcher" OFF)
option(VTK_DISPATCH_CONSTANT_ARRAYS "Include implicit vtkDataArray subclasses based on a constant backend in dispatcher" OFF)
option(VTK_DISPATCH_STD_FUNCTION_ARRAYS "Include implicit vtkDataArray subclasses based on std::function in dispatcher" OFF)
option(VTK_DISPATCH_STRIDED_ARRAYS "Include implicit vtkDataArray subclasses based on a strided backend in dispatcher" OFF)
option(VTK_DISPATCH_STRUCTURED_POINT_ARRAYS "Include implicit vtkDataArray subclasses based on structured point backend dispatcher" OFF)

option(VTK_WARN_ON_DISPATCH_FAILURE "If enabled, vtkArrayDispatch will print a warning when a dispatch fails." OFF)

# fvtk BUILD-TIME / SIZE lever: drop the two dead implicit-array families
# vtkStridedArray and vtkStdFunctionArray. PyVista never constructs either (no
# path in pyvista/_vtk.py, the reader factory, or numpy_to_vtk creates them), and
# their dispatch options (VTK_DISPATCH_STRIDED_ARRAYS / _STD_FUNCTION_ARRAYS) are
# OFF by default so they are already absent from the vtkArrayDispatch TypeList.
# What is still paid for them is the per-numeric-type template-instantiation TUs
# (vtkStridedArrayInstantiate_*, vtkStdFunctionArrayInstantiate_*,
# vtkStridedImplicitBackendInstantiate_*) plus the generated fixed-size
# specialization classes (vtkStridedType*Array / vtkStdFunctionType*Array). This
# drops exactly those. The template HEADERS (vtkStridedArray.h /
# vtkStdFunctionArray.h / vtkStridedImplicitBackend.h) are KEPT in place
# (header-only, zero compile cost when not instantiated) so the cut is purely an
# instantiation drop and trivially reversible. Unlike SOA/ScaledSOA, these two
# are NOT referenced as complete types by vtkDataArrayPrivate.txx or any kept
# CommonCore machinery (verified: no non-self include/reference in the tree), so
# dropping the instantiations is link-safe, not a compile error. Default ON;
# FVTK_DROP_DEAD_ARRAYS=OFF restores the stock instantiations.
option(FVTK_DROP_DEAD_ARRAYS
  "fvtk: drop instantiations of the PyVista-unused vtkStridedArray + vtkStdFunctionArray families"
  ON)

mark_as_advanced(
  VTK_DISPATCH_AOS_ARRAYS
  VTK_DISPATCH_SOA_ARRAYS
  VTK_DISPATCH_SCALED_SOA_ARRAYS

  VTK_DISPATCH_AFFINE_ARRAYS
  VTK_DISPATCH_CONSTANT_ARRAYS
  VTK_DISPATCH_STD_FUNCTION_ARRAYS
  VTK_DISPATCH_STRIDED_ARRAYS
  VTK_DISPATCH_STRUCTURED_POINT_ARRAYS

  VTK_WARN_ON_DISPATCH_FAILURE)


configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/vtkVTK_DISPATCH_IMPLICIT_ARRAYS.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkVTK_DISPATCH_IMPLICIT_ARRAYS.h"
  @ONLY)

include("${CMAKE_CURRENT_SOURCE_DIR}/vtkCreateArrayDispatchArrayList.cmake")
vtkArrayDispatch_default_array_setup()
vtkArrayDispatch_generate_array_header(VTK_ARRAYDISPATCH_ARRAY_LIST)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/vtkArrayDispatchArrayList.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkArrayDispatchArrayList.h"
  @ONLY)

# create a list of source files for explicit array type instantiations
set(instantiation_sources)

include(vtkTypeLists)

# fvtk BUILD-TIME lever: split the per-type "bulk" array instantiation TUs.
#
# Stock VTK concatenates ~17 template-instantiation sources (AOS/SOA/implicit
# backends, etc.) per numeric type into ONE vtkArrayBulkInstantiate_<type>.cxx
# via #include (14 such bulk TUs). FVTK_SPLIT_BULK_INSTANTIATE=ON instead compiles
# the ~312 component .cxx DIRECTLY, turning 14 large TUs into many small ones. It
# is byte-for-byte identical OBJECT CODE (same explicit instantiations + flags,
# just emitted into separate .o) -> ABI- and bit-exact-neutral; it only changes
# build scheduling.
#
# DEFAULT OFF — MEASURED net-NEGATIVE at the build-time level that matters.
# Each split component TU independently re-parses the heavy array template
# headers (vtkDataArrayPrivate.txx + all the AOS/SOA/implicit backend templates)
# that the bulk TU parsed ONCE per type, so the front-end/template cost is paid
# ~17x more. That extra total work is not recovered by better load-balancing.
# Measured (CommonCore array-instantiation TUs only, manylinux2014 GCC 10.2.1):
#     -j4 : bulk 248 s  vs  split 340 s   (split +37% SLOWER)
#     -j16: bulk  67 s  vs  split  90 s   (split +34% SLOWER)
# Both the 4-core CI target and a 16-core box favor BULK. (An earlier claim of
# "361->161 s, -55%" did not reproduce.) Kept as an opt-in for the unusual case
# of a very-high-core machine where the bulk path's 14-TU width is the ceiling;
# FVTK_SPLIT_BULK_INSTANTIATE=ON re-enables it (and builds clean on GCC < 11 via
# the VTK_DEPRECATION_LEVEL=0 fix on the split TUs below).
option(FVTK_SPLIT_BULK_INSTANTIATE
  "fvtk: compile per-type array instantiations as many small TUs instead of one bulk TU (measured slower at <=16 cores; default OFF)"
  OFF)

if (NOT FVTK_SPLIT_BULK_INSTANTIATE)
  foreach (type IN LISTS vtk_numeric_types)
    string(REPLACE " " "_" _suffix "${type}")
    list(APPEND instantiation_sources
      "${CMAKE_CURRENT_BINARY_DIR}/vtkArrayBulkInstantiate_${_suffix}.cxx")

    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|AppleClang|Clang)$")
      # This silences the following warning:
      #     type attributes ignored after type is already defined [-Wattributes]
      #     template class VTKCOMMONCORE_EXPORT vtkImplicitArray<vtkStructuredPointBackend<ValueType>>;
      # This warning is caused by the vtkArrayDispatch instantiations,
      # controlled by the VTK_DISPATCH_*_ARRAYS cmake variables.
      # When ON, a vtkImplicitArray<vtk*Backend<ValueType>> will be indirectly instantiated
      # by `vtkIndexedImplicitBackend::Internals` and `vtkCompositeImplicitBackend::Internals`
      # causing the `__attribute__((visibility("default")))` to be ignored.
      set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/vtkArrayBulkInstantiate_${_suffix}.cxx"
        PROPERTIES COMPILE_OPTIONS "-Wno-attributes")
    endif()
  endforeach ()
endif ()

foreach (INSTANTIATION_VALUE_TYPE IN LISTS vtk_numeric_types)
  # We never need to instantiate vtkIdType or because they are instantiated
  # by the corresponding array for its native type.
  if (INSTANTIATION_VALUE_TYPE STREQUAL "vtkIdType")
    continue()
  endif ()
  # create a file suffix from the type
  string(REPLACE " " "_" _suffix "${INSTANTIATION_VALUE_TYPE}")

  # Order of this list is important with bulk instantiation
  set(_list
    vtkAffineImplicitBackendInstantiate
    vtkCompositeImplicitBackendInstantiate
    vtkConstantImplicitBackendInstantiate
    vtkIndexedImplicitBackendInstantiate
    vtkStructuredPointBackendInstantiate
    vtkAffineArrayInstantiate
    vtkAOSDataArrayTemplateInstantiate
    vtkCompositeArrayInstantiate
    vtkConstantArrayInstantiate
    vtkIndexedArrayInstantiate
    vtkScaledSOADataArrayTemplateInstantiate
    vtkSOADataArrayTemplateInstantiate
    vtkStructuredPointArrayInstantiate
    vtkTypedDataArrayInstantiate)

  # The dead vtkStridedArray + vtkStdFunctionArray families: their
  # instantiation TUs are only added back when the cut is disabled
  # (FVTK_DROP_DEAD_ARRAYS=OFF). See the option above.
  if (NOT FVTK_DROP_DEAD_ARRAYS)
    list(APPEND _list
      vtkStridedImplicitBackendInstantiate
      vtkStridedArrayInstantiate
      vtkStdFunctionArrayInstantiate)
  endif ()

  # NOTE: do NOT try to drop the dispatch-disabled SOA/ScaledSOA (or implicit)
  # backend instantiations from this list to save size — they are referenced
  # directly by CommonCore's own array machinery (e.g. vtkDataArrayPrivate.txx
  # GetValueRange) regardless of the VTK_DISPATCH_*_ARRAYS switches, so removing
  # them is an incomplete-type COMPILE error, not just a link drop. (Validated
  # the hard way: removing SOA/ScaledSOA broke vtkDataArrayPrivate.txx.)

  # see comments in vtkGenericDataArray.h for explanation
  if (INSTANTIATION_VALUE_TYPE MATCHES "(^| )long( |$)")
    list(APPEND _list "vtkGenericDataArrayValueRangeInstantiate")
  endif ()

  # required for configure file
  set("bulk_instantiation_sources_${_suffix}")
  # generate cxx file to instantiate template with this type
  foreach (_prefix IN LISTS _list)
    configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/${_prefix}.cxx.in"
      "${CMAKE_CURRENT_BINARY_DIR}/${_prefix}_${_suffix}.cxx"
      @ONLY)

    if (FVTK_SPLIT_BULK_INSTANTIATE)
      # Compile each component instantiation as its own small TU (build-time
      # parallelism) instead of #include-ing it into the per-type bulk TU.
      list(APPEND instantiation_sources
        "${CMAKE_CURRENT_BINARY_DIR}/${_prefix}_${_suffix}.cxx")
      # VTK_DEPRECATION_LEVEL=0 reproduces EXACTLY what the bulk wrapper TU did:
      # vtkArrayBulkInstantiate.cxx.in opens with `#define VTK_DEPRECATION_LEVEL 0`
      # BEFORE any include, which makes VTK_DEPRECATED_IN_9_6_0() expand to empty
      # (vtkDeprecation.h gates it on the level). The deprecated implicit-array
      # specialization headers (vtkAffine<T>Array.h etc.) place the deprecation
      # attribute as `class EXPORT VTK_DEPRECATED_IN_9_6_0(...) Name : public Base`;
      # GCC < 11 (manylinux2014 ships GCC 10.2.1) CANNOT parse a [[deprecated]]
      # attribute in that position after the export macro -> hard parse error
      # ("expected identifier before '[' token"). Splitting these out of the bulk
      # wrapper loses the wrapper's level-0 #define, so we must reapply it per-TU.
      # This is byte-identical to the bulk object code (the attribute is suppressed
      # in BOTH paths and has zero codegen effect regardless). -Wno-attributes is
      # also kept for the dispatch-instantiation [-Wattributes] warning.
      if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|AppleClang|Clang)$")
        set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/${_prefix}_${_suffix}.cxx"
          PROPERTIES COMPILE_OPTIONS "-Wno-attributes"
                     COMPILE_DEFINITIONS "VTK_DEPRECATION_LEVEL=0")
      endif()
    else ()
      list(APPEND "bulk_instantiation_sources_${_suffix}"
        "#include \"${_prefix}_${_suffix}.cxx\"")
    endif ()
  endforeach ()
endforeach ()

set(classes
  vtkAbstractArray
  vtkAnimationCue
  vtkArchiver
  vtkArray
  vtkArrayComponents
  vtkArrayCoordinates
  vtkArrayExtents
  vtkArrayExtentsList
  vtkArrayIterator
  vtkArrayRange
  vtkArraySort
  vtkArrayWeights
  vtkAtomicMutex
  vtkBitArray
  vtkBitArrayIterator
  vtkBoxMuellerRandomSequence
  vtkBreakPoint
  vtkByteSwap
  vtkCallbackCommand
  vtkCharArray
  vtkCollection
  vtkCollectionIterator
  vtkCommand
  vtkCommonInformationKeyManager
  vtkDataArray
  vtkDataArrayCollection
  vtkDataArrayCollectionIterator
  vtkDataArraySelection
  vtkDebugLeaks
  vtkDebugLeaksManager
  vtkDeserializer
  vtkDoubleArray
  vtkDynamicLoader
  vtkEventForwarderCommand
  vtkFileOutputWindow
  vtkFloatArray
  vtkFloatingPointExceptions
  vtkGarbageCollector
  vtkGarbageCollectorManager
  vtkGaussianRandomSequence
  vtkIdList
  vtkIdListCollection
  vtkIdTypeArray
  vtkIndent
  vtkInformation
  vtkInformationDataObjectKey
  vtkInformationDoubleKey
  vtkInformationDoubleVectorKey
  vtkInformationIdTypeKey
  vtkInformationInformationKey
  vtkInformationInformationVectorKey
  vtkInformationIntegerKey
  vtkInformationIntegerPointerKey
  vtkInformationIntegerVectorKey
  vtkInformationIterator
  vtkInformationKey
  vtkInformationKeyLookup
  vtkInformationKeyVectorKey
  vtkInformationObjectBaseKey
  vtkInformationObjectBaseVectorKey
  vtkInformationRequestKey
  vtkInformationStringKey
  vtkInformationStringVectorKey
  vtkInformationUnsignedLongKey
  vtkInformationVariantKey
  vtkInformationVariantVectorKey
  vtkInformationVector
  vtkIntArray
  vtkInvoker
  vtkLargeInteger
  vtkLogger
  vtkLongArray
  vtkLongLongArray
  vtkLookupTable
  vtkMath
  vtkMarshalContext
  vtkMersenneTwister
  vtkMinimalStandardRandomSequence
  vtkMultiThreader
  vtkNumberToString
  vtkOStrStreamWrapper
  vtkOStreamWrapper
  vtkObject
  vtkObjectBase
  vtkObjectFactory
  vtkObjectFactoryCollection
  vtkOldStyleCallbackCommand
  vtkOutputWindow
  vtkOverrideInformation
  vtkOverrideInformationCollection
  vtkPoints
  vtkPoints2D
  vtkPriorityQueue
  vtkRandomPool
  vtkRandomSequence
  vtkReferenceCount
  vtkSerializer
  vtkScalarsToColors
  vtkShortArray
  vtkSignedCharArray
  vtkSmartPointerBase
  vtkSortDataArray
  vtkStdString
  vtkStringArray
  vtkStringOutputWindow
  vtkStringToken
  vtkTimePointUtility
  vtkTimeStamp
  vtkThreadedCallbackQueue
  vtkUnsignedCharArray
  vtkUnsignedIntArray
  vtkUnsignedLongArray
  vtkUnsignedLongLongArray
  vtkUnsignedShortArray
  vtkVariant
  vtkVariantArray
  vtkVersion
  vtkVoidArray
  vtkWeakPointerBase
  vtkWeakReference
  vtkWindow
  vtkXMLFileOutputWindow)

set(template_classes
  vtkAOSDataArrayTemplate
  vtkArrayDispatch
  vtkArrayInterpolate
  vtkArrayIteratorTemplate
  vtkArrayPrint
  vtkDenseArray
  vtkGenericDataArray
  vtkMappedDataArray
  vtkScaledSOADataArrayTemplate
  vtkSOADataArrayTemplate
  vtkSparseArray
  vtkThreadedTaskQueue
  vtkTypedArray
  vtkTypedDataArray)

set(nowrap_template_classes
  vtkCompositeImplicitBackend
  vtkImplicitArray
  vtkIndexedImplicitBackend
  vtkStridedImplicitBackend
  vtkStructuredPointBackend
  vtkTypeList)

# Hand written (de)serialization routines
set(serialization_helper_sources)
set(serialization_helper_headers)
if (VTK_WRAP_SERIALIZATION)
  list(APPEND serialization_helper_sources
    vtkDataArraySerDesHelper.cxx
    vtkInformationSerDesHelper.cxx
    vtkScalarsToColorsSerDesHelper.cxx
    vtkStringArraySerDesHelper.cxx
    vtkVariantArraySerDesHelper.cxx
    vtkVariantSerDesHelper.cxx)
  list(APPEND serialization_helper_headers
    vtkVariantSerDesHelper.h)
endif ()

set(sources
  vtkArrayIteratorTemplateInstantiate.cxx
  vtkFVTKSMPDefaults.cxx
  vtkGenericDataArray.cxx
  vtkStringFormatter.cxx
  vtkValueFromString.cxx

  vtkDataArray_CopyComponent.cxx
  vtkDataArray_DeepCopy.cxx
  vtkDataArray_FiniteScalarRange.cxx
  vtkDataArray_FiniteVectorRange.cxx
  vtkDataArray_GetTuples_ids.cxx
  vtkDataArray_GetTuples_range.cxx
  vtkDataArray_InsertTuples_array_idlist.cxx
  vtkDataArray_InsertTuples_array_idlist_range.cxx
  vtkDataArray_InsertTuples_array_range.cxx
  vtkDataArray_InterpolateTuple_time.cxx
  vtkDataArray_InterpolateTuple_weights.cxx
  vtkDataArray_ScalarRange.cxx
  vtkDataArray_SetTuple_array.cxx
  vtkDataArray_VectorRange.cxx

  ${serialization_helper_sources}
  ${instantiation_sources}
  ${vtk_smp_sources})

include(CheckCXXSourceCompiles)

function(CHECK_CXX_EXPRESSION_COMPILES EXPRESSION FILES VARIABLE)
  set(SOURCE "/* CHECK_CXX_EXPRESSION_COMPILES */\n")
  foreach (FILE ${FILES})
    string(APPEND SOURCE "#include <${FILE}>\n")
  endforeach ()
  string(APPEND SOURCE "\nint main()\n{\n")
  string(APPEND SOURCE "  static_cast<void>(${EXPRESSION});\n\n")
  string(APPEND SOURCE "  return 0;\n}\n")
  check_cxx_source_compiles("${SOURCE}" "${VARIABLE}")
endfunction()

# Check C++ <charconv> next, where the C++17 standard says these must be.
check_cxx_expression_compiles("std::chars_format()" "charconv" VTK_HAS_STD_CHARS_FORMAT)
check_cxx_expression_compiles("std::from_chars_result()" "charconv" VTK_HAS_STD_FROM_CHARS_RESULT)
check_cxx_expression_compiles("std::to_chars_result()" "charconv" VTK_HAS_STD_TO_CHARS_RESULT)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/vtkCharConvCompatibility.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkCharConvCompatibility.h")

set(headers
  vtkABI.h
  vtkArrayDispatchDataSetArrayList.h
  vtkArrayIteratorIncludes.h
  vtkAssume.h
  vtkAutoInit.h
  vtkBatch.h
  vtkBuffer.h
  vtkCompiler.h
  vtkDataArrayIteratorMacro.h
  vtkDataArrayMeta.h
  vtkDataArrayRange.h
  vtkDeprecation.h
  vtkEventData.h
  vtkGenericDataArrayLookupHelper.h
  vtkIOStream.h
  vtkIOStreamFwd.h
  vtkInformationInternals.h
  vtkMathUtilities.h
  vtkMatrixUtilities.h
  vtkMeta.h
  vtkNew.h
  vtkRange.h
  vtkRangeIterableTraits.h
  vtkSetGet.h
  vtkSmartPointer.h
  vtkStringFormatter.h
  vtkStringScanner.h
  vtkSystemIncludes.h
  vtkTemplateAliasMacro.h
  vtkTestDataArray.h
  vtkThreadedTaskQueue.h
  vtkType.h
  vtkTypeTraits.h
  vtkTypedDataArrayIterator.h
  vtkValueFromString.h
  vtkVariantCast.h
  vtkVariantCreate.h
  vtkVariantExtract.h
  vtkVariantInlineOperators.h
  vtkWeakPointer.h
  vtkWin32Header.h
  vtkWindows.h
  vtkWrappingHints.h
  ${vtk_smp_headers}
  ${serialization_helper_headers}
  "${CMAKE_CURRENT_BINARY_DIR}/vtkABINamespace.h"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkArrayDispatchArrayList.h"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkCharConvCompatibility.h"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkMathConfigure.h"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkTypeListMacros.h")

set(private_headers
  "${CMAKE_CURRENT_BINARY_DIR}/vtkFloatingPointExceptionsConfigure.h")

set(templates
  vtkArrayIteratorTemplateImplicit.txx
  ${vtk_smp_templates})

set(private_templates
  vtkAffineImplicitBackend.txx
  vtkDataArrayPrivate.txx)

set(vtk_include_dirs)

vtk_module_find_package(
  PACKAGE Threads)

set(VTK_MAX_THREADS "64" CACHE STRING
  "Max number of threads vtkMultiThreader will allocate.")
mark_as_advanced(VTK_MAX_THREADS)

if (CMAKE_USE_PTHREADS_INIT AND CMAKE_USE_WIN32_THREADS_INIT)
  if (DEFINED _vtk_thread_impl_output)
    set(thread_impl_output "${_vtk_thread_impl_output}")
  else ()
    execute_process(
      COMMAND "${CMAKE_C_COMPILER}" -v
      OUTPUT_VARIABLE thread_impl_output
      ERROR_VARIABLE  thread_impl_output
      RESULT_VARIABLE result
      TIMEOUT         10)
    if (result)
      message(FATAL_ERROR
        "Failed to determine the thread implementation in use by the "
        "compiler.")
    endif ()
    string (REGEX REPLACE ".*Thread model: \((posix|win32)\).*" "\\1" thread_impl_output "${thread_impl_output}")
  endif ()
  set(_vtk_thread_impl_output "${thread_impl_output}"
    CACHE INTERNAL "thread model detection")
  if (thread_impl_output MATCHES "posix")
    set(VTK_USE_PTHREADS 1)
  elseif (thread_impl_output MATCHES "win32")
    set(VTK_USE_WIN32_THREADS 1)
  else ()
    set(VTK_USE_PTHREADS 1)
  endif ()
elseif (CMAKE_USE_PTHREADS_INIT)
  set(VTK_USE_PTHREADS 1)
elseif (CMAKE_USE_WIN32_THREADS_INIT)
  set(VTK_USE_WIN32_THREADS 1)
endif ()

# Honor `VTK_WEBASSEMBLY_THREADS` option.
if (CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  set(VTK_USE_PTHREADS ${VTK_WEBASSEMBLY_THREADS})
endif ()

if(WIN32)
  list(APPEND classes
    vtkWin32OutputWindow
    vtkWin32ProcessOutputWindow)
  list(APPEND sources
    vtkWin32OutputWindowProcessEncoded.c)
endif()

if(ANDROID)
  list(APPEND classes
    vtkAndroidOutputWindow)
endif()

# Platform configuration tests.
include(TestBigEndian)
test_big_endian(VTK_WORDS_BIGENDIAN)

# vtkSMP header information.
set(smp_backends
  OpenMP
  Sequential
  STDThread
  TBB)
foreach (smp_backend IN LISTS smp_backends)
  string(TOLOWER "${smp_backend}" smp_backend_lower)
  string(TOUPPER "${smp_backend}" smp_backend_upper)
  set("VTK_SMP_ENABLE_${smp_backend_upper}" "${vtk_smp_enable_${smp_backend_lower}}")
  if (VTK_SMP_IMPLEMENTATION_TYPE STREQUAL smp_backend)
    set("VTK_SMP_DEFAULT_IMPLEMENTATION_${smp_backend_upper}" 1)
  else ()
    set("VTK_SMP_DEFAULT_IMPLEMENTATION_${smp_backend_upper}" 0)
  endif ()
endforeach ()

if (VTK_BUILD_VERSION LESS "20000101")
  set(VTK_EPOCH_VERSION "${VTK_BUILD_VERSION}")
else ()
  set(VTK_EPOCH_VERSION "20250514")
endif ()

set(nowrap_headers
  vtkAffineArray.h
  vtkAffineImplicitBackend.h
  vtkFVTKSMPDefaults.h
  vtkCollectionRange.h
  vtkCompositeArray.h
  vtkConstantArray.h
  vtkConstantImplicitBackend.h
  vtkDataArrayAccessor.h
  vtkDataArrayTupleRange_AOS.h
  vtkDataArrayTupleRange_Generic.h
  vtkDataArrayValueRange_AOS.h
  vtkDataArrayValueRange_Generic.h
  vtkHashCombiner.h
  vtkImplicitArrayTraits.h
  vtkIndexedArray.h
  vtkInherits.h
  vtkMathPrivate.hxx
  vtkStridedArray.h
  vtkStdFunctionArray.h
  vtkStructuredPointArray.h
  vtkTypeName.h
  ${vtk_smp_nowrap_headers}
  "${CMAKE_CURRENT_BINARY_DIR}/vtkVTK_DISPATCH_IMPLICIT_ARRAYS.h"
)
set(generated_headers
  vtkBuild.h
  vtkDebug.h
  vtkDebugRangeIterators.h
  vtkEndian.h
  vtkFeatures.h
  vtkLegacy.h
  vtkOptions.h
  vtkPlatform.h
  vtkSMP.h
  vtkThreads.h
  vtkVersionFull.h
  vtkVersionMacros.h
  vtkVersionQuick.h)
if (CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  list(APPEND generated_headers
    vtkSMPWebAssembly.h)
endif()
foreach (generated_header IN LISTS generated_headers)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/${generated_header}.in"
    "${CMAKE_CURRENT_BINARY_DIR}/${generated_header}"
    @ONLY)
  list(APPEND headers
    "${CMAKE_CURRENT_BINARY_DIR}/${generated_header}")
endforeach ()

include("${CMAKE_CURRENT_SOURCE_DIR}/vtkTypeArrays.cmake")

# Only assemble the per-type bulk wrapper TUs when NOT splitting (split mode
# compiles the component .cxx directly; see FVTK_SPLIT_BULK_INSTANTIATE above).
if (NOT FVTK_SPLIT_BULK_INSTANTIATE)
  foreach (type IN LISTS vtk_numeric_types)
    # Referenced in vtkArrayBulkInstantiate.cxx.in
    string(REPLACE " " "_" _suffix "${type}")
    list(JOIN "bulk_instantiation_sources_${_suffix}" "\n" BULK_INSTANTIATION_SOURCES)
    configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/vtkArrayBulkInstantiate.cxx.in"
      "${CMAKE_CURRENT_BINARY_DIR}/vtkArrayBulkInstantiate_${_suffix}.cxx"
      @ONLY)
    unset(BULK_INSTANTIATION_SOURCES)
  endforeach()
endif ()

# Check C++ <cmath> first, where the C++11 standard says these must be.
check_cxx_expression_compiles("std::isnan(0.0)" "cmath" VTK_HAS_STD_ISNAN)
check_cxx_expression_compiles("std::isinf(0.0)" "cmath" VTK_HAS_STD_ISINF)
check_cxx_expression_compiles("std::isfinite(0.0)" "cmath" VTK_HAS_STD_ISFINITE)

include(CheckSymbolExists)

# Check C99 <math.h> next, where the C99 standard says these must be.
# (they will be found even if they are defined as macros)
check_symbol_exists(isnan "math.h" VTK_HAS_ISNAN)
check_symbol_exists(isinf "math.h" VTK_HAS_ISINF)
check_symbol_exists(isfinite "math.h" VTK_HAS_ISFINITE)

# Check for finite() which is widespread, though non-standard.
check_symbol_exists(finite "math.h" VTK_HAS_FINITE)

# Check float.h last, where Windows keeps its underscore versions.
if(NOT VTK_HAS_ISNAN AND NOT VTK_HAS_STD_ISNAN)
  check_symbol_exists(_isnan "float.h" VTK_HAS__ISNAN)
endif()
if(NOT VTK_HAS_FINITE AND NOT VTK_HAS_ISFINITE AND NOT VTK_HAS_STD_ISFINITE)
  check_symbol_exists(_finite "float.h" VTK_HAS__FINITE)
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/vtkMathConfigure.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkMathConfigure.h")

# Figure out if feenableexcept and fedisableexcept functions are present
include(CheckIncludeFile)
check_include_file("fenv.h" HAVE_FENV_H)
if(HAVE_FENV_H)
  set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
  if(UNIX)
    set(CMAKE_REQUIRED_LIBRARIES m)
  endif()
  check_symbol_exists(feenableexcept "fenv.h" VTK_HAS_FEENABLEEXCEPT)
  set(CMAKE_REQUIRED_LIBRARIES)
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/vtkFloatingPointExceptionsConfigure.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkFloatingPointExceptionsConfigure.h")

include(CheckCXXSymbolExists)

# Determine whether the cxxabi header exists and if it contains a demangle function.
# XXX(c++17): prefer `__has_include`
check_include_file_cxx("cxxabi.h" HAVE_CXXABI_H)
if (HAVE_CXXABI_H)
  check_cxx_symbol_exists(abi::__cxa_demangle "cxxabi.h" VTK_HAS_CXXABI_DEMANGLE)
endif()
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/vtkCxxABIConfigure.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkCxxABIConfigure.h")
list(APPEND headers "${CMAKE_CURRENT_BINARY_DIR}/vtkCxxABIConfigure.h")

set_property(SOURCE vtkObjectBase.cxx
  PROPERTY
     COMPILE_DEFINITIONS ${vtk_object_base_defines})
set_property(SOURCE vtkLogger.cxx
  PROPERTY
    COMPILE_DEFINITIONS "LOGURU_SCOPE_TIME_PRECISION=${VTK_LOGGING_TIME_PRECISION}")

if (MSVC)
  set_property(SOURCE vtkDataArray.cxx APPEND
    PROPERTY
      COMPILE_OPTIONS /bigobj)
elseif (MINGW)
  check_cxx_compiler_flag("-Wa,-mbig-obj" VTK_HAS_MBIG_OBJ)
  mark_as_advanced(VTK_HAS_MBIG_OBJ)
  if (VTK_HAS_MBIG_OBJ)
    set_property(SOURCE vtkDataArray.cxx APPEND
      PROPERTY
        COMPILE_OPTIONS "-Wa,-mbig-obj")
  endif ()
endif ()

vtk_module_add_module(VTK::CommonCore
  HEADER_DIRECTORIES
  CLASSES           ${classes}
  TEMPLATE_CLASSES  ${template_classes}
  NOWRAP_TEMPLATE_CLASSES ${nowrap_template_classes}
  SOURCES           ${sources}
  TEMPLATES         ${templates}
  HEADERS           ${headers}
  NOWRAP_HEADERS    ${nowrap_headers}
  PRIVATE_HEADERS   ${private_headers}
  PRIVATE_TEMPLATES ${private_templates})
vtk_add_test_mangling(VTK::CommonCore)
if (vtk_include_dirs)
  vtk_module_include(VTK::CommonCore
    PRIVATE
      ${vtk_include_dirs})
endif ()

vtk_module_link(VTK::CommonCore
  PUBLIC
    Threads::Threads
    ${vtk_smp_libraries}
    ${vtk_memkind_libraries}
  PRIVATE
    # Need nsl to resolve gethostbyname on SunOS-5.8 and socket also
    $<$<PLATFORM_ID:SunOS>:socket>
    $<$<PLATFORM_ID:SunOS>:nsl>
    $<$<PLATFORM_ID:Android>:log>
    )
vtk_module_compile_features(VTK::CommonCore
  PUBLIC
    cxx_std_17)
