set(classes
  vtkAdaptiveDataSetSurfaceFilter
  vtkPolyDataSilhouette
  vtkRenderLargeImage)

vtk_module_add_module(VTK::FiltersHybridRendering
  CLASSES ${classes})
vtk_add_test_mangling(VTK::FiltersHybridRendering)
