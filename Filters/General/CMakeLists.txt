set(classes
  vtkAnimateModes
  vtkAnnotationLink
  vtkAppendLocationAttributes
  vtkAppendPoints
  vtkApproximatingSubdivisionFilter
  vtkAreaContourSpectrumFilter
  vtkAxes
  vtkAxisAlignedReflectionFilter
  vtkAxisAlignedTransformFilter
  vtkBlankStructuredGrid
  vtkBlankStructuredGridWithImage
  vtkBlockIdScalars
  vtkBooleanOperationPolyDataFilter
  vtkBoxClipDataSet
  vtkBrownianPoints
  vtkCellDerivatives
  vtkCellValidator
  vtkCleanUnstructuredGrid
  vtkCleanUnstructuredGridCells
  vtkClipClosedSurface
  vtkClipConvexPolyData
  vtkClipDataSet
  vtkClipVolume
  vtkCoincidentPoints
  vtkContourTriangulator
  vtkCountFaces
  vtkCountVertices
  vtkCursor2D
  vtkCursor3D
  vtkCurvatures
  vtkDataSetGradient
  vtkDataSetGradientPrecompute
  vtkDataSetTriangleFilter
  vtkDateToNumeric
  vtkDeflectNormals
  vtkDeformPointSet
  vtkDensifyPolyData
  vtkDicer
  vtkDiscreteFlyingEdges2D
  vtkDiscreteFlyingEdges3D
  vtkDiscreteFlyingEdgesClipper2D
  vtkDiscreteMarchingCubes
  vtkDistancePolyDataFilter
  vtkEdgePoints
  vtkEqualizerFilter
  vtkExplodeDataSet
  vtkExtractArray
  vtkExtractGhostCells
  vtkExtractSelectedFrustum
  vtkExtractSelectionBase
  vtkFiniteElementFieldDistributor
  vtkGradientFilter
  vtkGraphLayoutFilter
  vtkGraphToPoints
  vtkGraphWeightEuclideanDistanceFilter
  vtkGraphWeightFilter
  vtkGroupDataSetsFilter
  vtkGroupTimeStepsFilter
  vtkHierarchicalDataLevelFilter
  vtkHyperStreamline
  vtkIconGlyphFilter
  vtkImageDataToPointSet
  vtkImageMarchingCubes
  vtkInterpolateDataSetAttributes
  vtkInterpolatingSubdivisionFilter
  vtkIntersectionPolyDataFilter
  vtkJoinTables
  vtkLevelIdScalars
  vtkLinkEdgels
  vtkLoopBooleanPolyDataFilter
  vtkMarchingContourFilter
  vtkMatricizeArray
  vtkMergeArrays
  vtkMergeCells
  vtkMergeTimeFilter
  vtkMergeVectorComponents
  vtkMultiBlockDataGroupFilter
  vtkMultiBlockMergeFilter
  vtkMultiThreshold
  vtkNormalizeMatrixVectors
  vtkOBBDicer
  vtkOBBTree
  vtkOverlappingAMRLevelIdScalars
  vtkPassArrays
  vtkPassSelectedArrays
  vtkPointConnectivityFilter
  vtkPointsMatchingTransformFilter
  vtkPolyDataStreamer
  vtkPolyDataToReebGraphFilter
  vtkProbePolyhedron
  vtkQuadraturePointInterpolator
  vtkQuadraturePointsGenerator
  vtkQuadratureSchemeDictionaryGenerator
  vtkQuantizePolyDataPoints
  vtkRandomAttributeGenerator
  vtkRectilinearGridClip
  vtkRectilinearGridToPointSet
  vtkRectilinearGridToTetrahedra
  vtkRecursiveDividingCubes
  vtkReflectionFilter
  vtkRemovePolyData
  vtkRotationFilter
  vtkSampleImplicitFunctionFilter
  vtkShrinkFilter
  vtkShrinkPolyData
  vtkSpatialRepresentationFilter
  vtkSpatioTemporalHarmonicsAttribute
  vtkSphericalHarmonics
  vtkSplineFilter
  vtkSplitByCellScalarFilter
  vtkSplitColumnComponents
  vtkSplitField
  vtkStructuredGridClip
  vtkSumTables
  vtkSubPixelPositionEdgels
  vtkSubdivisionFilter
  vtkSynchronizeTimeFilter

  vtkTableBasedClipDataSet
  vtkTableFFT
  vtkTableToPolyData
  vtkTableToStructuredGrid
  vtkTemporalPathLineFilter
  vtkTemporalStatistics
  vtkTessellatorFilter
  vtkTimeSourceExample
  vtkTransformFilter
  vtkTransformPolyDataFilter
  vtkUncertaintyTubeFilter
  vtkVertexGlyphFilter
  vtkVolumeContourSpectrumFilter
  vtkVoxelContoursToSurfaceFilter
  vtkWarpLens
  vtkWarpScalar
  vtkWarpTo
  vtkWarpVector
  vtkYoungsMaterialInterface)

set(templates)

set(no_wrap_classes
  vtkReflectionUtilities)

# fvtk SIMD: vtkWarpVector.cxx carries the target_clones("default","avx2")
# multi-versioned warp kernel (fvtkWarpVectorRange). xo = xi + sf*v is the
# canonical a*b+c FMA shape; FMA contraction would diverge from stock VTK 9.6.2
# by 1 ULP on adversarial data, so this TU MUST compile with -ffp-contract=off
# for the avx2 (and default) clone to stay bit-exact. The global build does not
# set -ffp-contract, so set it here.
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set_source_files_properties(vtkWarpVector.cxx
    PROPERTIES
      COMPILE_OPTIONS -ffp-contract=off)
endif()

vtk_module_add_module(VTK::FiltersGeneral
  CLASSES ${classes}
  NOWRAP_CLASSES ${no_wrap_classes}
  TEMPLATES ${templates})
vtk_add_test_mangling(VTK::FiltersGeneral)
