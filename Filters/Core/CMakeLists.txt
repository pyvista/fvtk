set(classes
  vtk3DLinearGridCrinkleExtractor
  vtk3DLinearGridPlaneCutter
  vtkAppendArcLength
  vtkAppendCompositeDataLeaves
  vtkAppendDataSets
  vtkAppendFilter
  vtkAppendPartitionedDataSetCollection
  vtkAppendPolyData
  vtkAppendSelection
  vtkArrayCalculator
  vtkArrayRename
  vtkAssignAttribute
  vtkAttributeDataToFieldDataFilter
  vtkAttributeDataToTableFilter
  vtkBinCellDataFilter
  vtkBinnedDecimation
  vtkCellCenters
  vtkCellDataToPointData
  vtkCenterOfMass
  vtkCleanPolyData
  vtkClipPolyData
  vtkCompositeCutter
  vtkCompositeDataProbeFilter
  vtkConnectivityFilter
  vtkConstrainedSmoothingFilter
  vtkContour3DLinearGrid
  vtkContourFilter
  vtkContourGrid
  vtkContourHelper
  vtkConvertToMultiBlockDataSet
  vtkConvertToPartitionedDataSetCollection
  vtkConvertToPolyhedra
  vtkCutter
  vtkDataObjectGenerator
  vtkDataObjectToDataSetFilter
  vtkDataSetEdgeSubdivisionCriterion
  vtkDataSetToDataObjectFilter
  vtkDecimatePolylineAngleStrategy
  vtkDecimatePolylineCustomFieldStrategy
  vtkDecimatePolylineDistanceStrategy
  vtkDecimatePolylineFilter
  vtkDecimatePro
  vtkDelaunay2D
  vtkDelaunay3D
  vtkEdgeSubdivisionCriterion
  vtkElevationFilter
  vtkExecutionTimer
  vtkExplicitStructuredGridCrop
  vtkExplicitStructuredGridToUnstructuredGrid
  vtkExtractCells
  vtkExtractCellsAlongPolyLine
  vtkExtractEdges
  vtkFeatureEdges
  vtkFieldDataToAttributeDataFilter
  vtkFieldDataToDataSetAttribute
  vtkFlyingEdges2D
  vtkFlyingEdges3D
  vtkFlyingEdgesPlaneCutter
  vtkGenerateIds
  vtkGenerateRegionIds
  vtkGlyph2D
  vtkGlyph3D
  vtkGridSynchronizedTemplates3D
  vtkHedgeHog
  vtkHull
  vtkHyperTreeGridProbeFilter
  vtkHyperTreeGridProbeFilterUtilities
  vtkImageAppend
  vtkImageDataToExplicitStructuredGrid
  vtkImplicitPolyDataDistance
  vtkImplicitProjectOnPlaneDistance
  vtkMarchingCubes
  vtkMarchingSquares
  vtkMaskFields
  vtkMaskPoints
  vtkMaskPolyData
  vtkMassProperties
  vtkMergeDataObjectFilter
  vtkMergeFields
  vtkMergeFilter
  vtkMoleculeAppend
  vtkMultiObjectMassProperties
  vtkOrientPolyData
  vtkPackLabels
  vtkPassThrough
  vtkPlaneCutter
  vtkPointDataToCellData
  vtkPolyDataConnectivityFilter
  vtkPolyDataEdgeConnectivityFilter
  vtkPolyDataNormals
  vtkPolyDataPlaneClipper
  vtkPolyDataPlaneCutter
  vtkPolyDataTangents
  vtkPolyDataToUnstructuredGrid
  vtkProbeFilter
  vtkQuadricClustering
  vtkQuadricDecimation
  vtkRearrangeFields
  vtkRectilinearSynchronizedTemplates
  vtkRemoveDuplicatePolys
  vtkRemoveUnusedPoints
  vtkResampleToImage
  vtkResampleWithDataSet
  vtkReverseSense
  vtkSimpleElevationFilter
  vtkSmoothPolyDataFilter
  vtkSphereTreeFilter
  vtkSplitSharpEdgesPolyData
  vtkStructuredDataPlaneCutter
  vtkStaticCleanPolyData
  vtkStaticCleanUnstructuredGrid
  vtkStreamerBase
  vtkStreamingTessellator
  vtkStripper
  vtkStructuredGridAppend
  vtkStructuredGridOutlineFilter
  vtkSurfaceNets2D
  vtkSurfaceNets3D
  vtkSynchronizedTemplates2D
  vtkSynchronizedTemplates3D
  vtkSynchronizedTemplatesCutter3D
  vtkTensorGlyph
  vtkThreshold
  vtkThresholdPoints
  vtkThresholdScalars
  vtkTransposeTable
  vtkTriangleFilter
  vtkTriangleMeshPointNormals
  vtkTubeBender
  vtkTubeFilter
  vtkUnstructuredGridQuadricDecimation
  vtkUnstructuredGridToExplicitStructuredGrid
  vtkVectorDot
  vtkVectorNorm
  vtkVoronoi2D
  vtkWindowedSincPolyDataFilter)

set(headers
  vtkDecimatePolylineStrategy.h)

set(private_headers
  vtk3DLinearGridInternal.h
  vtkPolyDataEdgeNeighbors.h)

vtk_module_add_module(VTK::FiltersCore
  CLASSES ${classes}
  HEADERS ${headers}
  SOURCES fvtkFastClean.cxx fvtkFastCleanPoly.cxx fvtkFastConnectivity.cxx
  PRIVATE_HEADERS ${private_headers}
    fvtkFastClean.h fvtkFastCleanPoly.h fvtkFastConnectivity.h
    pvaClean.h pvaHash96.h pvaLinearProbeTable.h pvaParallelRadixSort.h)

vtk_add_test_mangling(VTK::FiltersCore)

# compile those TUs with OpenMP when available, and keep them out of the unity
# build so the vendored header stays isolated. Without OpenMP the adapters
# compile to stubs (the Fast* entry points return false -> standard path).
#
# The vendored kernel uses OpenMP 4.0 constructs (`for simd`) and GCC/Clang
# atomic/prefetch builtins; MSVC's default `/openmp` is only 2.0, so we do NOT
# enable the fast lane on MSVC -- the adapters compile to the bit-exact stub and
# Windows falls back to stock vtkCleanPolyData / vtkStaticClean (no behavioural
# change, the fast lane is opt-in to begin with).
set_source_files_properties(fvtkFastClean.cxx fvtkFastCleanPoly.cxx fvtkFastConnectivity.cxx
  PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
find_package(OpenMP QUIET)
if (OpenMP_CXX_FOUND AND NOT MSVC)
  vtk_module_link(VTK::FiltersCore PRIVATE OpenMP::OpenMP_CXX)
  set_property(SOURCE fvtkFastClean.cxx fvtkFastCleanPoly.cxx fvtkFastConnectivity.cxx
    APPEND PROPERTY COMPILE_DEFINITIONS FVTK_HAVE_OPENMP=1)
endif ()
