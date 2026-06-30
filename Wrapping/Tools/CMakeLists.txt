# Allow the user to customize their build with some local options
include("${CMAKE_CURRENT_SOURCE_DIR}/LocalUserOptions.cmake" OPTIONAL)

set(ns "VTK")
if (VTK_BUILD_COMPILE_TOOLS_ONLY)
  set(ns "VTKCompileTools")
endif ()

set(sources
  vtkParse.tab.c
  vtkParseAttributes.c
  vtkParseData.c
  vtkParseDepends.c
  vtkParseExtras.c
  vtkParseHierarchy.c
  vtkParseMain.c
  vtkParseMangle.c
  vtkParseMerge.c
  vtkParseMethodType.c
  vtkParsePreprocess.c
  vtkParseProperties.c
  vtkParseString.c
  vtkParseSystem.c
  vtkWrap.c
  vtkWrapText.c)

set(headers
  vtkParse.h
  vtkParseAttributes.h
  vtkParseData.h
  vtkParseDepends.h
  vtkParseExtras.h
  vtkParseHierarchy.h
  vtkParseMain.h
  vtkParseMangle.h
  vtkParseMerge.h
  vtkParseMethodType.h
  vtkParsePreprocess.h
  vtkParseProperties.h
  vtkParseString.h
  vtkParseSystem.h
  vtkParseType.h
  vtkWrap.h
  vtkWrapText.h)

set_property(SOURCE vtkParseMain.c
  PROPERTY
    COMPILE_DEFINITIONS "VTK_PARSE_VERSION=\"${VTK_MAJOR_VERSION}.${VTK_MINOR_VERSION}\"")

# Disable abi mangling in this module
set(_vtk_module_no_namespace_abi_mangling ON)

vtk_module_add_module("${ns}::WrappingTools"
  SOURCES ${sources}
  HEADERS ${headers})
if (CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  vtk_module_compile_options("${ns}::WrappingTools"
    PRIVATE
      "-pthread")
endif ()

if (NOT TARGET "${ns}::WrapHierarchy")
  vtk_module_add_executable(WrapHierarchy
    DEVELOPMENT
    BASENAME vtkWrapHierarchy
    vtkWrapHierarchy.c)
  target_link_libraries(WrapHierarchy
    PRIVATE
      "${ns}::WrappingTools")
  if (CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    target_compile_options(WrapHierarchy
      PRIVATE
        "-pthread")
    # allows vtkWrapHierarchy to access host file system for headers, generated sources, etc.
    # permit memory growth to generate arbitrary sized hierarchy files.
    target_link_options(WrapHierarchy
      PRIVATE
        "-pthread"
        # Remove after https://github.com/WebAssembly/design/issues/1271 is closed
        "-Wno-pthreads-mem-growth"
        "-sSINGLE_FILE=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sNODERAWFS=1")
    set_target_properties(WrapHierarchy
      PROPERTIES
        SUFFIX ".cjs")
  endif ()
  add_executable("${ns}::WrapHierarchy" ALIAS
    WrapHierarchy)
endif ()

if (NOT TARGET WrapPython)
  vtk_module_add_executable(WrapPython
    DEVELOPMENT
    BASENAME vtkWrapPython
    vtkWrapPython.c
    vtkWrapPythonClass.c
    vtkWrapPythonConstant.c
    vtkWrapPythonEnum.c
    vtkWrapPythonMethod.c
    vtkWrapPythonMethodDef.c
    vtkWrapPythonNamespace.c
    vtkWrapPythonNumberProtocol.c
    vtkWrapPythonOverload.c
    vtkWrapPythonProperty.c
    vtkWrapPythonTemplate.c
    vtkWrapPythonType.c)
  target_link_libraries(WrapPython
    PRIVATE
      "${ns}::WrappingTools")
  add_executable("${ns}::WrapPython" ALIAS
    WrapPython)

  vtk_module_add_executable(WrapPythonInit
    DEVELOPMENT
    BASENAME vtkWrapPythonInit
    vtkWrapPythonInit.c)
  target_link_libraries(WrapPythonInit
    PRIVATE
      "${ns}::WrappingTools")
  add_executable("${ns}::WrapPythonInit" ALIAS
    WrapPythonInit)
endif ()

# cvista: the Java / JavaScript / SerDes wrapper-generator tools have been removed.
# This wheel is Python-only (VTK_WRAP_PYTHON=YES; VTK_WRAP_JAVA, VTK_WRAP_JAVASCRIPT
# and VTK_WRAP_SERIALIZATION are unsupported), so the ParseJava/WrapJava/WrapSerDes/
# WrapJavaScript executables and their sources (vtkWrapJava*, vtkParseJava,
# vtkWrapSerDes*, vtkWrapJavaScript*) are gone. Only the Python wrapper generator
# and the shared parser (vtkParse*) remain.
