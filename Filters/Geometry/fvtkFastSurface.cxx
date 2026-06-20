// SPDX-License-Identifier: BSD-3-Clause
// fvtk opt-in fast boundary-surface extraction: VTK adapter over the vendored
// pyvista-algorithms OpenMP kernel (pvaExtractSurface.h, MIT). This TU is built
// with OpenMP and excluded from the unity build so the vendored code stays
// isolated.
#include "fvtkFastSurface.h"

#include "vtkFVTKSMPDefaults.h" // fvtk::FastModeEnabled (needed even without OpenMP)

#ifdef FVTK_HAVE_OPENMP

#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkCellType.h"
#include "vtkDoubleArray.h"
#include "vtkFloatArray.h"
#include "vtkIdTypeArray.h"
#include "vtkNew.h"
#include "vtkPointData.h"
#include "vtkPoints.h"
#include "vtkPolyData.h"
#include "vtkTypeInt32Array.h"
#include "vtkTypeInt64Array.h"
#include "vtkUnsignedCharArray.h"
#include "vtkUnstructuredGrid.h"

#include "pvaExtractSurface.h" // vendored MIT kernel (namespace fse); pulls in <omp.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
// Int32 view of a vtkCellArray index array (offsets / connectivity). Zero-copy
// when already int32 storage (fvtk's width-relaxed default); otherwise narrow.
const std::int32_t* AsInt32(vtkDataArray* a, std::vector<std::int32_t>& buf)
{
  if (auto* p = vtkTypeInt32Array::SafeDownCast(a))
  {
    return p->GetPointer(0);
  }
  const vtkIdType n = a->GetNumberOfValues();
  buf.resize(static_cast<size_t>(n));
  if (auto* p = vtkTypeInt64Array::SafeDownCast(a))
  {
    const long long* s = p->GetPointer(0);
    for (vtkIdType i = 0; i < n; ++i)
    {
      buf[i] = static_cast<std::int32_t>(s[i]);
    }
  }
  else
  {
    for (vtkIdType i = 0; i < n; ++i)
    {
      buf[i] = static_cast<std::int32_t>(a->GetComponent(i, 0));
    }
  }
  return buf.data();
}
}

#endif // FVTK_HAVE_OPENMP

namespace fvtk
{
VTK_ABI_NAMESPACE_BEGIN

bool FastUnstructuredSurface(vtkUnstructuredGrid* input, vtkPolyData* output, bool passPointIds,
  bool passCellIds, const char* pointIdsName, const char* cellIdsName)
{
  if (!FastModeEnabled())
  {
    return false;
  }
#ifndef FVTK_HAVE_OPENMP
  // Built without OpenMP -> fast path unavailable; caller uses the standard path.
  (void)input;
  (void)output;
  (void)passPointIds;
  (void)passCellIds;
  (void)pointIdsName;
  (void)cellIdsName;
  return false;
#else

  vtkCellArray* cells = input->GetCells();
  vtkPoints* pts = input->GetPoints();
  if (!cells || !pts || !pts->GetData())
  {
    return false;
  }
  const vtkIdType nCells = input->GetNumberOfCells();
  const vtkIdType nPts = input->GetNumberOfPoints();
  if (nCells == 0 || nPts == 0 || nPts > static_cast<vtkIdType>(0x7FFFFFFF))
  {
    return false;
  }

  // Per-cell type array. GetCellTypesArray() returns null for homogeneous grids
  // (types stored as an implicit vtkConstantArray, e.g. SetCells(int, cells)), so
  // fall back to a per-cell copy via GetCellType() when the AOS pointer is absent.
  std::vector<unsigned char> typeBuf;
  const unsigned char* ct;
  if (vtkUnsignedCharArray* ta = input->GetCellTypesArray())
  {
    ct = ta->GetPointer(0);
  }
  else
  {
    typeBuf.resize(static_cast<size_t>(nCells));
    for (vtkIdType i = 0; i < nCells; ++i)
    {
      typeBuf[i] = static_cast<unsigned char>(input->GetCellType(i));
    }
    ct = typeBuf.data();
  }

  // Only supported linear 3D cell types -- otherwise fall back to the standard path.
  for (vtkIdType i = 0; i < nCells; ++i)
  {
    switch (ct[i])
    {
      case VTK_TETRA:
      case VTK_HEXAHEDRON:
      case VTK_VOXEL:
      case VTK_WEDGE:
      case VTK_PYRAMID:
        break;
      default:
        return false;
    }
  }

  vtkDataArray* ptData = pts->GetData();
  const int ptType = ptData->GetDataType();
  if ((ptType != VTK_FLOAT && ptType != VTK_DOUBLE) || ptData->GetNumberOfComponents() != 3)
  {
    return false;
  }
  const bool isDouble = (ptType == VTK_DOUBLE);

  std::vector<std::int32_t> offBuf, connBuf;
  const std::int32_t* off32 = AsInt32(cells->GetOffsetsArray(), offBuf);
  const std::int32_t* conn32 = AsInt32(cells->GetConnectivityArray(), connBuf);

  // Vendored kernel (OpenMP). n_threads=0 -> OpenMP default (OMP_NUM_THREADS).
  fse::ExtractResult res = fse::extract_surface(
    conn32, off32, ct, static_cast<std::int32_t>(nCells), static_cast<std::int32_t>(nPts), 0);
  if (isDouble)
  {
    fse::compact_points_f64(res, static_cast<const double*>(ptData->GetVoidPointer(0)),
      static_cast<std::int32_t>(nPts), 0);
  }
  else
  {
    fse::compact_points(res, static_cast<const float*>(ptData->GetVoidPointer(0)),
      static_cast<std::int32_t>(nPts), 0);
  }

  const vtkIdType nTri = static_cast<vtkIdType>(res.tri_indices.size() / 3);
  const vtkIdType nQuad = static_cast<vtkIdType>(res.quad_indices.size() / 4);
  const vtkIdType nOut = nTri + nQuad;
  const vtkIdType nUsed = res.n_points_used;

  // Output points (preserve input precision; coordinates are exact input copies).
  vtkNew<vtkPoints> newPts;
  newPts->SetDataType(ptType);
  newPts->SetNumberOfPoints(nUsed);
  if (nUsed > 0)
  {
    if (isDouble)
    {
      std::memcpy(newPts->GetVoidPointer(0), res.points_f64.data(),
        sizeof(double) * 3 * static_cast<size_t>(nUsed));
    }
    else
    {
      std::memcpy(newPts->GetVoidPointer(0), res.points.data(),
        sizeof(float) * 3 * static_cast<size_t>(nUsed));
    }
  }
  output->SetPoints(newPts);

  // Polygons: all tris, then all quads (cell data follows the same order).
  vtkNew<vtkCellArray> polys;
  polys->AllocateExact(nOut, nTri * 3 + nQuad * 4);
  vtkIdType ids[4];
  for (vtkIdType i = 0; i < nTri; ++i)
  {
    ids[0] = res.tri_indices[3 * i];
    ids[1] = res.tri_indices[3 * i + 1];
    ids[2] = res.tri_indices[3 * i + 2];
    polys->InsertNextCell(3, ids);
  }
  for (vtkIdType i = 0; i < nQuad; ++i)
  {
    ids[0] = res.quad_indices[4 * i];
    ids[1] = res.quad_indices[4 * i + 1];
    ids[2] = res.quad_indices[4 * i + 2];
    ids[3] = res.quad_indices[4 * i + 3];
    polys->InsertNextCell(4, ids);
  }
  output->SetPolys(polys);

  // Point data: copy each surface point from its original (point_map[i]).
  vtkPointData* inPD = input->GetPointData();
  vtkPointData* outPD = output->GetPointData();
  outPD->CopyAllocate(inPD, nUsed);
  for (vtkIdType i = 0; i < nUsed; ++i)
  {
    outPD->CopyData(inPD, res.point_map[i], i);
  }

  // Cell data: copy each surface cell from its origin cell (tris then quads).
  vtkCellData* inCD = input->GetCellData();
  vtkCellData* outCD = output->GetCellData();
  outCD->CopyAllocate(inCD, nOut);
  vtkIdType oc = 0;
  for (vtkIdType i = 0; i < nTri; ++i)
  {
    outCD->CopyData(inCD, res.tri_origin_cell[i], oc++);
  }
  for (vtkIdType i = 0; i < nQuad; ++i)
  {
    outCD->CopyData(inCD, res.quad_origin_cell[i], oc++);
  }

  if (passPointIds)
  {
    vtkNew<vtkIdTypeArray> origPts;
    origPts->SetName(pointIdsName ? pointIdsName : "vtkOriginalPointIds");
    origPts->SetNumberOfValues(nUsed);
    for (vtkIdType i = 0; i < nUsed; ++i)
    {
      origPts->SetValue(i, res.point_map[i]);
    }
    outPD->AddArray(origPts);
  }
  if (passCellIds)
  {
    vtkNew<vtkIdTypeArray> origCells;
    origCells->SetName(cellIdsName ? cellIdsName : "vtkOriginalCellIds");
    origCells->SetNumberOfValues(nOut);
    oc = 0;
    for (vtkIdType i = 0; i < nTri; ++i)
    {
      origCells->SetValue(oc++, res.tri_origin_cell[i]);
    }
    for (vtkIdType i = 0; i < nQuad; ++i)
    {
      origCells->SetValue(oc++, res.quad_origin_cell[i]);
    }
    outCD->AddArray(origCells);
  }

  return true;
#endif // FVTK_HAVE_OPENMP
}

VTK_ABI_NAMESPACE_END
} // namespace fvtk
