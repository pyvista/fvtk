// SPDX-License-Identifier: BSD-3-Clause
// cvista opt-in fast polygonal clip for vtkClipPolyData.
//
// Replaces vtkClipPolyData's serial per-cell clip loop (each cell calls
// vtkCell::Clip into ONE shared incremental vtkMergePoints -- an order-locked
// serial merge that is the measured hotspot of pyvista's surface-clip path)
// with a threaded per-cell clip into thread-local outputs, then a coincident-
// point merge of the composited fragments. Kept in a separate translation unit
// (excluded from the unity build) so its threading/kernel internals stay
// isolated, mirroring cvistaFastClean / cvistaFastCleanPoly / cvistaFastConnectivity.
#ifndef cvistaFastClipPoly_h
#define cvistaFastClipPoly_h

#include "vtkABINamespace.h"

VTK_ABI_NAMESPACE_BEGIN
class vtkPolyData;
class vtkDataArray;
class vtkPointData;
class vtkCellData;
VTK_ABI_NAMESPACE_END

namespace cvista
{
VTK_ABI_NAMESPACE_BEGIN

/**
 * Opt-in fast polygonal clip for vtkClipPolyData's common surface-clip regime
 * (a polys-only triangulated/polygonal surface, single output, no second
 * "clipped" output).
 *
 * No-op (returns false, caller runs the stock serial loop) unless
 * cvista::FastModeEnabled() (env CVISTA_FAST / cvista.EnableFast()) AND the request is
 * the supported regime:
 *   - @p input is non-null and has ONLY polys (no verts/lines/strips), so every
 *     output cell is dimension 2 and routes to newPolys -- there is no
 *     Verts/Lines emission to reproduce,
 *   - @p input has at least one cell.
 *
 * @p clipScalars is the per-INPUT-point scalar array the caller already computed
 * (ClipFunction->FunctionValue(point) per point, or the input scalars); @p value
 * is vtkClipPolyData::Value; @p insideOut is vtkClipPolyData::InsideOut.
 * @p inPD / @p inCD are the EXACT point/cell-data objects the caller passes to
 * vtkCell::Clip (note: with a clip function the caller replaces inPD with a
 * shallow copy that may carry the generated clip scalars -- we use whatever it
 * hands us). @p copyScalars mirrors the caller's outPD CopyScalars On/Off
 * decision so the thread-local interpolated point data is allocated identically
 * to stock.
 *
 * On success it fills @p output with the clipped surface (points + polys +
 * interpolated point data + passed-through-by-cell cell data) the stock serial
 * loop would have produced, and returns true; the caller then skips the serial
 * loop and its output assembly. The output is POINTS-relaxed: the SAME point set
 * and the SAME polys (as a multiset), but points are renumbered in a
 * thread-/merge-dependent order rather than serial insertion order.
 */
// Intra-module linkage (defined in cvistaFastClipPoly.cxx, called from
// vtkClipPolyData.cxx within the same shared library).
bool FastClipPolyData(vtkPolyData* input, vtkPolyData* output, vtkDataArray* clipScalars,
  double value, int insideOut, vtkPointData* inPD, vtkCellData* inCD, bool copyScalars);

VTK_ABI_NAMESPACE_END
} // namespace cvista

#endif
// VTK-HeaderTest-Exclude: cvistaFastClipPoly.h
