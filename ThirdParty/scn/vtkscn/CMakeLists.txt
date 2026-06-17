set(headers
  include/vtkscn/chrono.h
  include/vtkscn/fwd.h
  include/vtkscn/istream.h
  include/vtkscn/ranges.h
  include/vtkscn/regex.h
  include/vtkscn/scan.h
  include/vtkscn/xchar.h
  "${CMAKE_CURRENT_BINARY_DIR}/include/vtkscn/scn_export.h"
)

set(private_headers
  src/vtkscn/impl.h)

set(private_sources
  src/vtkscn/impl.cpp)

vtk_module_add_module(VTK::scn
  HEADER_DIRECTORIES
  HEADERS ${headers}
  SOURCES ${private_sources}
  PRIVATE_HEADERS ${private_headers}
  HEADERS_SUBDIR "vtkscn")

# fvtk build-time trim: the only consumer of scn in fvtk's module closure is
# Common/Core/vtkStringScanner.h (vtk::scan / vtk::from_chars), which scans
# *narrow* numeric/string values only. It exposes no regex API and no
# locale-aware ("L"-flag / scan(locale,...)) scanning. The default scnlib
# config instantiates the full std::regex backend and the locale machinery in
# the single monolithic impl.cpp TU (the heaviest non-dispatch TU in the
# build). Disabling the two unused feature families is correctness-neutral for
# fvtk's usage and removes ~26% of impl.cpp's compile work (measured: object
# code 4.02 MB -> 2.90 MB, ~-26% CPU on a warm -O3 build). These macros are
# PUBLIC because they gate inline code in scan.h that consumers parse, so they
# must agree across impl.cpp and every consumer TU for ABI consistency.
vtk_module_definitions(VTK::scn
  PUBLIC
    SCN_DISABLE_REGEX=1   # fvtk uses no regex scanning; drops the std::regex backend
    SCN_DISABLE_LOCALE=1  # fvtk uses no locale-aware scanning ('L' flag / scan(locale,...))
    SCN_REGEX_BACKEND=0 # std::regex
    SCN_DISABLE_FAST_FLOAT=0)

# include directories for the public and private headers
vtk_module_include(VTK::scn
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)

include(GenerateExportHeader)
generate_export_header(scn
  EXPORT_MACRO_NAME SCN_EXPORT
  EXPORT_FILE_NAME include/vtkscn/scn_export.h)

set_target_properties(scn PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

# scn includes its own headers with `<>`, so we need an explicit `-I` for
# them to find each other.
vtk_module_include(VTK::scn
  INTERFACE
    "$<INSTALL_INTERFACE:${_vtk_build_HEADERS_DESTINATION}/vtkscn/include>")
