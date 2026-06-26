// SPDX-License-Identifier: BSD-3-Clause
// cvista opt-in fast coincident-point merge.
//
// Thin VTK adapter over the vendored pyvista-algorithms OpenMP point-dedup
// kernel (pvaClean.h, MIT). Kept in a separate translation unit so the vendored
// code and its <omp.h> dependency are isolated from the rest of the
// (unity-built) FiltersCore module.
#ifndef cvistaFastClean_h
#define cvistaFastClean_h

#include "vtkABINamespace.h"

VTK_ABI_NAMESPACE_BEGIN
class vtkUnstructuredGrid;
VTK_ABI_NAMESPACE_END

namespace cvista
{
VTK_ABI_NAMESPACE_BEGIN

/**
 * Opt-in fast coincident-point merge for a vtkUnstructuredGrid (the
 * vtkStaticCleanUnstructuredGrid fast path).
 *
 * No-op (returns false) unless cvista::FastModeEnabled() (env CVISTA_FAST /
 * cvista.EnableFast()) AND the request is the exact-merge default regime:
 *   - @p effectiveTolerance == 0 (exact coincident merge only),
 *   - @p averagePointData == false (kernel copies the canonical point, no avg),
 *   - @p hasMergingArray == false (no merge-by-data-array),
 *   - the grid is concrete with float/double points, <2^31 points, and NO
 *     polyhedra (kernel reports a fallback for those).
 * On success it fills @p output with the merged grid (points deduped +
 * compacted, connectivity rewritten, cells kept 1:1 in input order so cell data
 * passes through unchanged) and returns true; the caller then skips the standard
 * path. The output is POINTS-relaxed: the same merged point set and the same
 * cells, but points are renumbered in a thread-/hash-dependent order.
 */
// Intra-module linkage (defined in cvistaFastClean.cxx, called from
// vtkStaticCleanUnstructuredGrid.cxx within the same shared library).
bool FastStaticCleanUnstructuredGrid(vtkUnstructuredGrid* input, vtkUnstructuredGrid* output,
  double effectiveTolerance, bool removeUnusedPoints, bool averagePointData, bool hasMergingArray,
  int outputPointsPrecision);

VTK_ABI_NAMESPACE_END
} // namespace cvista

#endif
// VTK-HeaderTest-Exclude: cvistaFastClean.h
