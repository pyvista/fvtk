// SPDX-License-Identifier: BSD-3-Clause
// fvtk opt-in fast parallel triangle-mesh decimation (the vtkDecimatePro fast
// path). Mirrors fvtkFastClean.h.
//
// A parallel MIS-wave half-edge-collapse decimator that fvtk routes
// vtkDecimatePro through ONLY when fvtk::FastModeEnabled() is true. The kernel
// lives in its own translation unit (excluded from the unity build) so its
// VTK_SIMPLE_VERTEX / VTK_BOUNDARY_VERTEX ... macros (copied from
// vtkDecimatePro.cxx) cannot clash with the unity-built TUs.
//
// GATE ("order unimportant, points matter"):
//   - output points are an EXACT subset of input point positions (collapses
//     only ever copy an existing input coordinate; no point is moved/created),
//   - output point/cell ORDER is free (renumbered in a thread-dependent order),
//   - the requested TargetReduction is reached within a small tolerance, else
//     the partial result is DISCARDED and false is returned (the host then runs
//     the serial vtkDecimatePro so the user always gets the correct reduction).
#ifndef fvtkFastDecimate_h
#define fvtkFastDecimate_h

#include "vtkABINamespace.h"

#include <cstdint>

VTK_ABI_NAMESPACE_BEGIN
class vtkPolyData;
VTK_ABI_NAMESPACE_END

namespace fvtk
{
VTK_ABI_NAMESPACE_BEGIN

/**
 * Opt-in fast decimation for an all-triangle vtkPolyData (the vtkDecimatePro
 * fast path).
 *
 * No-op (returns false) unless fvtk::FastModeEnabled() (env FVTK_FAST /
 * fvtk.EnableFast()) AND the request is in the supported v1 regime:
 *   - @p preserveTopology == false (the kernel never splits),
 *   - the input is concrete all-triangle vtkPolyData with float/double points,
 *     <2^31 points/cells, and no verts/lines/strips,
 *   - @p targetReduction in (0, 1).
 * The kernel performs only SIMPLE and (when @p boundaryVertexDeletion) BOUNDARY
 * edge collapses; @p maximumError caps the per-vertex error and @p featureAngle
 * marks feature edges off-limits (conservative). On success it fills @p output
 * with the decimated mesh (points are an exact subset of the input, renumbered
 * in a thread-dependent order; cells renumbered to match) and returns true; the
 * caller then skips the serial path. If the achieved reduction falls short of
 * the target (or the input is unsupported), it leaves @p output untouched and
 * returns false so the caller runs the serial decimator.
 */
// Intra-module linkage (defined in fvtkFastDecimate.cxx, called from
// vtkDecimatePro.cxx within the same shared library).
bool FastDecimatePro(vtkPolyData* input, vtkPolyData* output, double targetReduction,
  double featureAngle, bool boundaryVertexDeletion, bool preserveTopology, double maximumError,
  int outputPointsPrecision);

/**
 * Number of times FastDecimatePro() has successfully engaged (taken the fast
 * path and returned true) in this process. Monotonically increasing; used by
 * the regression tests to prove the fast path actually ran (vs falling back to
 * serial). Exposed to Python through vtkDecimatePro::GetFastModeEngageCount().
 */
std::uint64_t GetFastDecimateEngageCount();

VTK_ABI_NAMESPACE_END
} // namespace fvtk

#endif
// VTK-HeaderTest-Exclude: fvtkFastDecimate.h
