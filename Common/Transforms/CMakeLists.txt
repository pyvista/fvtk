set(classes
  vtkAbstractTransform
  vtkCylindricalTransform
  vtkGeneralTransform
  vtkHomogeneousTransform
  vtkIdentityTransform
  vtkLandmarkTransform
  vtkLinearTransform
  vtkMatrixToHomogeneousTransform
  vtkMatrixToLinearTransform
  vtkPerspectiveTransform
  vtkSphericalTransform
  vtkThinPlateSplineTransform
  vtkTransform
  vtkTransform2D
  vtkTransformCollection
  vtkWarpTransform)

# cvista SIMD: vtkLinearTransform.cxx carries the target_clones("default","avx2")
# multi-versioned 4x4 matrix*point kernel (cvistaLinearTransformPointRange). The
# matrix*point math is the canonical a*b+c FMA shape; FMA contraction would
# diverge from stock VTK 9.6.2 by 1 ULP on adversarial data, so this TU MUST be
# compiled with -ffp-contract=off for the avx2 (and default) clone to stay
# bit-exact. The global build does NOT set -ffp-contract, so set it here.
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set_source_files_properties(vtkLinearTransform.cxx
    PROPERTIES
      COMPILE_OPTIONS -ffp-contract=off)
endif()

vtk_module_add_module(VTK::CommonTransforms
  CLASSES ${classes})
vtk_add_test_mangling(VTK::CommonTransforms)
