// SPDX-License-Identifier: BSD-3-Clause
// fvtk opt-in fast boundary-surface extraction.
//
// Thin VTK adapter over the vendored pyvista-algorithms OpenMP kernel
// (pvaExtractSurface.h, MIT). Kept in a separate translation unit so the
// vendored code and its <omp.h> dependency are isolated from the rest of the
// (unity-built) FiltersGeometry module.
#ifndef fvtkFastSurface_h
#define fvtkFastSurface_h

#include "vtkABINamespace.h"

VTK_ABI_NAMESPACE_BEGIN
class vtkUnstructuredGrid;
class vtkPolyData;
VTK_ABI_NAMESPACE_END

namespace fvtk
{
VTK_ABI_NAMESPACE_BEGIN

/**
 * Opt-in fast boundary-surface extraction for a vtkUnstructuredGrid.
 *
 * No-op (returns false) unless fvtk::FastModeEnabled() (env FVTK_FAST /
 * fvtk.EnableFast()) AND the grid is a concrete vtkUnstructuredGrid made
 * entirely of supported linear 3D cells (tetra/hex/voxel/wedge/pyramid) with
 * float or double points and <2^31 points. On success it fills @p output with
 * the boundary surface (points, polys, copied point/cell data, and optional
 * OriginalPointIds/CellIds) and returns true; the caller then skips the
 * standard path. The output is ORDER-RELAXED: the same surface, but cells and
 * surface points are emitted in a thread-/hash-dependent order.
 */
// Intra-module linkage (defined in fvtkFastSurface.cxx, called from
// vtkDataSetSurfaceFilter.cxx within the same shared library) -- no export needed.
bool FastUnstructuredSurface(vtkUnstructuredGrid* input, vtkPolyData* output, bool passPointIds,
  bool passCellIds, const char* pointIdsName, const char* cellIdsName);

VTK_ABI_NAMESPACE_END
} // namespace fvtk

#endif
// VTK-HeaderTest-Exclude: fvtkFastSurface.h
