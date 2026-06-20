// SPDX-License-Identifier: BSD-3-Clause
// fvtk opt-in fast connected-components labeling: parallel union-find over cells
// replacing vtkConnectivityFilter's serial wave-BFS (TraverseAndMark). Built
// with OpenMP and excluded from the unity build so the kernel + <omp.h> stay
// isolated, mirroring fvtkFastClean.
#include "fvtkFastConnectivity.h"

#include "vtkFVTKSMPDefaults.h" // fvtk::FastModeEnabled (needed even without OpenMP)

#ifdef FVTK_HAVE_OPENMP

#include "vtkDataSet.h"
#include "vtkIdList.h"
#include "vtkIdTypeArray.h"
#include "vtkNew.h"
#include "vtkStaticCellLinksTemplate.h"

#include <vector>

#include <omp.h>

namespace
{
// Concurrent disjoint-set. parent[] entries are accessed with relaxed atomics so
// the lock-free find/union are race-free under the C++ memory model. Union links
// the LARGER root to the SMALLER root, so every component's final root is its
// MINIMUM cell index -- which makes the region numbering below (ranking roots by
// ascending index) bit-identical to stock vtkConnectivityFilter, whose BFS seeds
// regions in increasing-unvisited-cell-index order.

inline vtkIdType ufLoad(vtkIdType* parent, vtkIdType x)
{
  return __atomic_load_n(&parent[x], __ATOMIC_RELAXED);
}

// Find with path halving (the halving stores are benign: they only ever move an
// entry closer to its root, monotonically).
inline vtkIdType ufFind(vtkIdType* parent, vtkIdType x)
{
  vtkIdType p = ufLoad(parent, x);
  while (p != x)
  {
    const vtkIdType gp = ufLoad(parent, p);
    if (gp != p)
    {
      __atomic_store_n(&parent[x], gp, __ATOMIC_RELAXED);
    }
    x = p;
    p = gp;
  }
  return x;
}

inline void ufUnion(vtkIdType* parent, vtkIdType a, vtkIdType b)
{
  for (;;)
  {
    a = ufFind(parent, a);
    b = ufFind(parent, b);
    if (a == b)
    {
      return;
    }
    const vtkIdType hi = a > b ? a : b;
    const vtkIdType lo = a < b ? a : b;
    vtkIdType expected = hi;
    // Link larger root hi -> smaller root lo. Succeeds only if parent[hi] is
    // still its own root (hi); otherwise someone changed it -> retry.
    if (__atomic_compare_exchange_n(
          &parent[hi], &expected, lo, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
    {
      return;
    }
  }
}
} // namespace

namespace fvtk
{
VTK_ABI_NAMESPACE_BEGIN

bool FastConnectivityAllRegions(vtkDataSet* input, vtkIdType numPts, vtkIdType numCells,
  bool scalarConnectivity, vtkIdType* visited, vtkIdType* pointMap, vtkIdTypeArray* newScalars,
  vtkIdTypeArray* newCellScalars, vtkIdTypeArray* regionSizes, vtkIdType& pointNumber,
  vtkIdType& regionNumber)
{
  if (!FastModeEnabled())
  {
    return false;
  }
  // Geometric connectivity only: the kernel unions cells that merely share a
  // point and does not evaluate the scalar-range criterion.
  if (scalarConnectivity)
  {
    return false;
  }
  if (input == nullptr || numCells < 1 || numPts < 1)
  {
    return false;
  }
  // 2^31 ceiling: region ids are stored as float (matching stock) and the int32
  // width-relaxed default keeps ids in range; bail to stock past that.
  if (numCells >= static_cast<vtkIdType>(1) << 31 ||
    numPts >= static_cast<vtkIdType>(1) << 31)
  {
    return false;
  }

  // Point -> incident-cell links (threaded build, all dataset types).
  vtkStaticCellLinksTemplate<vtkIdType> links;
  links.BuildLinks(input);

  std::vector<vtkIdType> parent(static_cast<size_t>(numCells));
#pragma omp parallel for schedule(static)
  for (vtkIdType i = 0; i < numCells; ++i)
  {
    parent[i] = i;
  }

  // Union every pair of cells incident to a common point. Unioning the first
  // incident cell with each of the others transitively connects them all.
#pragma omp parallel for schedule(dynamic, 4096)
  for (vtkIdType p = 0; p < numPts; ++p)
  {
    const vtkIdType nc = links.GetNumberOfCells(p);
    if (nc < 2)
    {
      continue;
    }
    const vtkIdType* cells = links.GetCells(p);
    const vtkIdType c0 = cells[0];
    for (vtkIdType j = 1; j < nc; ++j)
    {
      ufUnion(parent.data(), c0, cells[j]);
    }
  }

  // Fully flatten: every cell points directly at its component root (= min cell
  // index in the component).
#pragma omp parallel for schedule(static)
  for (vtkIdType i = 0; i < numCells; ++i)
  {
    parent[i] = ufFind(parent.data(), i);
  }

  // Rank roots in ascending index order -> region id. Roots appear at i where
  // parent[i]==i; scanning i ascending visits them in increasing min-cell-index,
  // exactly reproducing stock's region numbering. Reuse `visited` as the
  // root->regionId map (a cell that is its own root stores its region id; the
  // second pass below overwrites every entry with its cell's region id).
  regionNumber = 0;
  for (vtkIdType i = 0; i < numCells; ++i)
  {
    if (parent[i] == i)
    {
      visited[i] = regionNumber++;
    }
  }
  // regionSizes[r] = number of cells in region r.
  regionSizes->SetNumberOfTuples(regionNumber);
  for (vtkIdType r = 0; r < regionNumber; ++r)
  {
    regionSizes->SetValue(r, 0);
  }
  // Assign each cell its region id (= region id of its root) and accumulate
  // sizes + cell scalars.
  vtkIdType* cellScalarPtr = newCellScalars->GetPointer(0);
  vtkIdType* regionSizePtr = regionSizes->GetPointer(0);
  for (vtkIdType i = 0; i < numCells; ++i)
  {
    const vtkIdType region = visited[parent[i]];
    visited[i] = region;
    cellScalarPtr[i] = region;
    ++regionSizePtr[region];
  }

  // Point numbering: a single serial pass in cell-index order. Each point is
  // assigned a compacted output index the first time a cell references it, and
  // tagged with that cell's region id. (Point order therefore tracks cell index
  // rather than BFS encounter order -> POINTS-relaxed vs stock, same values.)
  vtkNew<vtkIdList> cellPts;
  vtkIdType* pointScalarPtr = newScalars->GetPointer(0);
  pointNumber = 0;
  for (vtkIdType i = 0; i < numCells; ++i)
  {
    input->GetCellPoints(i, cellPts);
    const vtkIdType npts = cellPts->GetNumberOfIds();
    const vtkIdType* ids = cellPts->GetPointer(0);
    const vtkIdType region = visited[i];
    for (vtkIdType k = 0; k < npts; ++k)
    {
      const vtkIdType pt = ids[k];
      if (pointMap[pt] < 0)
      {
        pointMap[pt] = pointNumber;
        pointScalarPtr[pointNumber] = region;
        ++pointNumber;
      }
    }
  }

  return true;
}

VTK_ABI_NAMESPACE_END
} // namespace fvtk

#else // !FVTK_HAVE_OPENMP -- bit-exact stub, caller runs the stock wave-BFS.

namespace fvtk
{
VTK_ABI_NAMESPACE_BEGIN

bool FastConnectivityAllRegions(vtkDataSet*, vtkIdType, vtkIdType, bool, vtkIdType*, vtkIdType*,
  vtkFloatArray*, vtkFloatArray*, vtkIdTypeArray*, vtkIdType&, vtkIdType&)
{
  return false;
}

VTK_ABI_NAMESPACE_END
} // namespace fvtk

#endif
