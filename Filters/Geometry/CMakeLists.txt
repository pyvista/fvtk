set(classes
  vtkAbstractGridConnectivity
  vtkAttributeSmoothingFilter
  vtkCompositeDataGeometryFilter
  vtkDataSetRegionSurfaceFilter
  vtkDataSetSurfaceFilter
  vtkExplicitStructuredGridSurfaceFilter
  vtkGeometryFilter
  vtkHierarchicalDataSetGeometryFilter
  vtkImageDataGeometryFilter
  vtkImageDataToUniformGrid
  vtkLinearToQuadraticCellsFilter
  vtkMarkBoundaryFilter
  vtkProjectSphereFilter
  vtkRecoverGeometryWireframe
  vtkRectilinearGridGeometryFilter
  vtkRectilinearGridPartitioner
  vtkStructuredAMRGridConnectivity
  vtkStructuredAMRNeighbor
  vtkStructuredGridConnectivity
  vtkStructuredGridGeometryFilter
  vtkStructuredGridPartitioner
  vtkStructuredNeighbor
  vtkStructuredPointsGeometryFilter
  vtkUnstructuredGridGeometryFilter)

vtk_module_add_module(VTK::FiltersGeometry
  CLASSES ${classes}
  SOURCES fvtkFastSurface.cxx
  PRIVATE_HEADERS fvtkFastSurface.h pvaExtractSurface.h)
vtk_add_test_mangling(VTK::FiltersGeometry)

# fvtk opt-in fast boundary-surface kernel (vendored pyvista-algorithms, MIT):
# compile that one TU with OpenMP when available, and keep it out of the unity
# build so the vendored header stays isolated. Without OpenMP the adapter
# compiles to a stub (FastUnstructuredSurface returns false -> standard path).
set_source_files_properties(fvtkFastSurface.cxx PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
find_package(OpenMP QUIET)
if (OpenMP_CXX_FOUND)
  vtk_module_link(VTK::FiltersGeometry PRIVATE OpenMP::OpenMP_CXX)
  set_property(SOURCE fvtkFastSurface.cxx APPEND PROPERTY COMPILE_DEFINITIONS FVTK_HAVE_OPENMP=1)
endif ()
