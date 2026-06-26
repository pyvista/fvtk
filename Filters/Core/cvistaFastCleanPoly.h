// SPDX-License-Identifier: BSD-3-Clause
// cvista opt-in fast coincident-point merge for polygonal data.
//
// Thin VTK adapter over the vendored pyvista-algorithms OpenMP point-dedup
// kernel (pvaClean.h, MIT). Kept in a separate translation unit so the vendored
// code and its <omp.h> dependency are isolated from the rest of the
// (unity-built) FiltersCore module.
#ifndef cvistaFastCleanPoly_h
#define cvistaFastCleanPoly_h

#include "vtkABINamespace.h"

VTK_ABI_NAMESPACE_BEGIN
class vtkPolyData;
VTK_ABI_NAMESPACE_END

namespace cvista
{
VTK_ABI_NAMESPACE_BEGIN

/**
 * Opt-in fast coincident-point merge for a polys-only vtkPolyData (the
 * vtkCleanPolyData fast path).
 *
 * No-op (returns false) unless cvista::FastModeEnabled() (env CVISTA_FAST /
 * cvista.EnableFast()) AND the request is the exact-merge default regime restricted
 * to the common triangulated-surface case:
 *   - @p pointMerging == true and @p effectiveTolerance == 0 (exact merge),
 *   - the input has polygons and NO verts/lines/strips,
 *   - no point global-ids (those drive a different merge predicate) and no ghost
 *     points,
 *   - float/double points, <2^31 points,
 *   - and NO cell degenerates under the merge (vtkCleanPolyData would convert a
 *     collapsed poly to a line/vertex; the kernel does not, so we fall back).
 * On success it fills @p output with the merged surface (points deduped +
 * compacted, polys renumbered + kept 1:1 in input order so cell data passes
 * through) and returns true; the caller then skips the standard path. The output
 * is POINTS-relaxed: same merged point set and same polys, points renumbered in a
 * thread-/hash-dependent order.
 */
bool FastCleanPolyData(vtkPolyData* input, vtkPolyData* output, bool pointMerging,
  double effectiveTolerance, int outputPointsPrecision);

VTK_ABI_NAMESPACE_END
} // namespace cvista

#endif
// VTK-HeaderTest-Exclude: cvistaFastCleanPoly.h
