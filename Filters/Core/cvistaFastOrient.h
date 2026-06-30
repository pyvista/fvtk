// SPDX-License-Identifier: BSD-3-Clause
// cvista opt-in fast polygon-orientation pass.
//
// VTK adapter over the cvista parallel orientation kernel (pvaOrient.h): a
// deterministic, multithreaded replacement for vtkOrientPolyData's serial
// single-threaded BFS wave (TraverseAndOrder), which is the last fully serial,
// order-locked stage in the default vtkPolyDataNormals (Consistency=1) pipeline.
// Kept in a separate translation unit (excluded from the unity build) so its
// <omp.h> dependency stays isolated, mirroring cvistaFastClean / cvistaFastConnectivity.
#ifndef cvistaFastOrient_h
#define cvistaFastOrient_h

#include "vtkABINamespace.h"

VTK_ABI_NAMESPACE_BEGIN
class vtkPolyData;
VTK_ABI_NAMESPACE_END

namespace cvista
{
VTK_ABI_NAMESPACE_BEGIN

/**
 * Opt-in fast polygon-orientation pass for vtkOrientPolyData's Consistency path.
 *
 * No-op (returns false, caller runs the stock serial wave-BFS) unless
 * cvista::FastModeEnabled() (env CVISTA_FAST / cvista.EnableFast()) AND the request is
 * the supported regime:
 *   - @p consistency == true and @p autoOrient == false (the AutoOrientNormals
 *     "leftmost cell" seeding is NOT accelerated),
 *   - @p nonManifoldTraversal == false,
 *   - the mesh is MANIFOLD: every polygon edge is shared by at most two cells.
 *     A non-manifold edge (> 2 incident cells) reported by the kernel forces a
 *     fallback to the byte-exact serial path.
 *
 * @p output must already hold the mutable copy of the polys that the stock filter
 * builds (DeepCopy of input->GetPolys() into output, with links/cells built); on
 * success this routine reverses, in place on @p output, exactly the cells the
 * stock pass would, EXCEPT that the absolute winding chosen per connected
 * component is resolved by the component's lowest cell id rather than stock's BFS
 * seed order. The result is ORIENTATION-relaxed: positions, the cell-to-slot
 * mapping, and the point-id multiset of every cell are identical to stock;
 * adjacent cells are always mutually consistent; only a whole-component winding
 * flip may differ, deterministically and independently of thread count.
 *
 * @p input is the link/cell source used to compute the orientation; @p output is
 * the mesh actually reversed (they share the same connectivity by construction in
 * vtkOrientPolyData::RequestData).
 */
bool FastOrientPolyData(vtkPolyData* input, vtkPolyData* output, bool consistency,
  bool flipNormals, bool autoOrient, bool nonManifoldTraversal);

VTK_ABI_NAMESPACE_END
} // namespace cvista

#endif
// VTK-HeaderTest-Exclude: cvistaFastOrient.h
