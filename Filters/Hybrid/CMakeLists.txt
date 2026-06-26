set(classes
  vtkBSplineTransform
  vtkDepthSortPolyData
  vtkDSPFilterDefinition
  vtkDSPFilterGroup
  vtkEarthSource
  vtkFacetReader
  vtkForceTime
  vtkGenerateTimeSteps
  vtkGreedyTerrainDecimation
  vtkGridTransform
  vtkImageToPolyDataFilter
  vtkImplicitModeller
  vtkPCAAnalysisFilter
  vtkProcrustesAlignmentFilter
  vtkProjectedTerrainPath
  vtkTemporalArrayOperatorFilter
  vtkTemporalDataSetCache
  vtkTemporalFractal
  vtkTemporalInterpolator
  vtkTemporalShiftScale
  vtkTemporalSnapToTimeStep
  vtkTransformToGrid
  vtkWeightedTransformFilter)

vtk_module_add_module(VTK::FiltersHybrid
  CLASSES ${classes})
vtk_add_test_mangling(VTK::FiltersHybrid)
