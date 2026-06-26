set(classes
  vtkPLY
  vtkPLYReader
  vtkPLYWriter)

# Vendored third-party fast PLY parser (MIT, Vilya Harvey) backing the cvista
# binary-LE fast path in vtkPLYReader. Plain source, not a wrapped VTK class.
set(sources
  miniply.cxx)
set(private_headers
  miniply.h)

vtk_module_add_module(VTK::IOPLY
  CLASSES ${classes}
  SOURCES ${sources}
  PRIVATE_HEADERS ${private_headers})
vtk_add_test_mangling(VTK::IOPLY)
