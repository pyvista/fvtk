// SPDX-License-Identifier: BSD-3-Clause
// fvtk opt-in fast connected-components labeling for vtkConnectivityFilter.
//
// Replaces the serial wave-BFS region labeling (vtkConnectivityFilter::
// TraverseAndMark, the measured #1 self-time hotspot) with a parallel
// union-find over cells. Kept in a separate translation unit (excluded from the
// unity build) so its <omp.h> dependency stays isolated, mirroring fvtkFastClean.
#ifndef fvtkFastConnectivity_h
#define fvtkFastConnectivity_h

#include "vtkABINamespace.h"
#include "vtkType.h"

VTK_ABI_NAMESPACE_BEGIN
class vtkDataSet;
class vtkIdTypeArray;
VTK_ABI_NAMESPACE_END

namespace fvtk
{
VTK_ABI_NAMESPACE_BEGIN

/**
 * Opt-in fast region labeling for vtkConnectivityFilter's
 * VTK_EXTRACT_ALL_REGIONS path with geometric (point-sharing) connectivity.
 *
 * No-op (returns false, caller runs the stock wave-BFS) unless
 * fvtk::FastModeEnabled() (env FVTK_FAST / fvtk.EnableFast()) AND the request is
 * the supported regime:
 *   - extraction mode is ALL_REGIONS (no seeds / largest / specified),
 *   - geometric connectivity only (@p scalarConnectivity == false; the kernel
 *     does not evaluate the scalar range criterion),
 *   - the grid is concrete with < 2^31 cells/points.
 *
 * On success it fills, for ALL cells/points (every cell is in some region):
 *   - @p visited  [numCells]      : region id per cell (cell scalars source),
 *   - @p pointMap [numPts]        : compacted output index per input point, or
 *                                   -1 for points used by no cell,
 *   - @p newScalars               : region id per compacted point (resized),
 *   - @p newCellScalars           : region id per cell,
 *   - @p regionSizes              : cell count per region,
 *   - @p pointNumber, @p regionNumber : final counts.
 *
 * The region ids are BIT-IDENTICAL to the stock filter (both number regions by
 * increasing minimum cell index), so only the OUTPUT POINT ORDER differs
 * (points are numbered in cell-index encounter order rather than BFS order) ->
 * the result is POINTS-relaxed, not value-changed. The caller's downstream
 * output assembly (point/cell extraction, OrderRegionIds, AddRegionsIds) is
 * unchanged.
 */
bool FastConnectivityAllRegions(vtkDataSet* input, vtkIdType numPts, vtkIdType numCells,
  bool scalarConnectivity, vtkIdType* visited, vtkIdType* pointMap, vtkIdTypeArray* newScalars,
  vtkIdTypeArray* newCellScalars, vtkIdTypeArray* regionSizes, vtkIdType& pointNumber,
  vtkIdType& regionNumber);

VTK_ABI_NAMESPACE_END
} // namespace fvtk

#endif
// VTK-HeaderTest-Exclude: fvtkFastConnectivity.h
