// SPDX-License-Identifier: BSD-3-Clause
// cvista opt-in fast coincident-point merge: VTK adapter over the vendored
// pyvista-algorithms OpenMP point-dedup kernel (pvaClean.h, MIT). This TU is
// built with OpenMP and excluded from the unity build so the vendored code
// stays isolated.
#include "cvistaFastClean.h"

#include "vtkCVISTASMPDefaults.h" // cvista::FastModeEnabled (needed even without OpenMP)

#ifdef CVISTA_HAVE_OPENMP

#include "vtkAlgorithm.h" // SINGLE_PRECISION / DOUBLE_PRECISION
#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkCellType.h"
#include "vtkDataArray.h"
#include "vtkNew.h"
#include "vtkPointData.h"
#include "vtkPoints.h"
#include "vtkTypeInt32Array.h"
#include "vtkTypeInt64Array.h"
#include "vtkUnsignedCharArray.h"
#include "vtkUnstructuredGrid.h"

#include "pvaClean.h" // vendored MIT kernel (namespace pvu::clean); pulls in <omp.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
// Int32 view of a vtkCellArray index array (offsets / connectivity). Zero-copy
// when already int32 storage (cvista's width-relaxed default); otherwise narrow.
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

// Build the merged output from a run_clean result of point type T.
template <typename T>
void BuildOutput(const T* inPoints, vtkIdType nPts, vtkUnstructuredGrid* input,
  vtkUnstructuredGrid* output, bool removeUnusedPoints, int outputPointsPrecision,
  pvu::clean::CleanInput<T, std::int32_t>& in, int inPtType)
{
  using namespace pvu::clean;
  CleanOptions opts;
  opts.tolerance = 0.0; // exact coincident merge (gated by the caller)
  opts.absolute = false;
  opts.remove_unused_points = removeUnusedPoints;
  opts.remove_degenerate_cells = false; // VTK keeps every cell (only remaps ids)
  opts.drop_nonfinite_points = false;

  CleanOutput<T> out;
  run_clean(in, opts, out, 0 /* n_threads -> OMP default */);

  const vtkIdType nNew = static_cast<vtkIdType>(out.points.size() / 3);
  const vtkIdType nCells = input->GetNumberOfCells();

  // Output points (DEFAULT precision = input type; coordinates are exact copies
  // of the canonical merged point, so values are bit-identical to stock).
  int outType = inPtType;
  if (outputPointsPrecision == vtkAlgorithm::SINGLE_PRECISION)
  {
    outType = VTK_FLOAT;
  }
  else if (outputPointsPrecision == vtkAlgorithm::DOUBLE_PRECISION)
  {
    outType = VTK_DOUBLE;
  }
  (void)inPoints;
  vtkNew<vtkPoints> newPts;
  newPts->SetDataType(outType);
  newPts->SetNumberOfPoints(nNew);
  if (nNew > 0)
  {
    if (outType == inPtType)
    {
      std::memcpy(
        newPts->GetVoidPointer(0), out.points.data(), sizeof(T) * 3 * static_cast<size_t>(nNew));
    }
    else
    {
      for (vtkIdType i = 0; i < nNew; ++i)
      {
        newPts->SetPoint(i, out.points[3 * i], out.points[3 * i + 1], out.points[3 * i + 2]);
      }
    }
  }
  output->SetPoints(newPts);

  // Point data: copy each merged point from its canonical original (source_map).
  vtkPointData* inPD = input->GetPointData();
  vtkPointData* outPD = output->GetPointData();
  outPD->CopyAllocate(inPD, nNew);
  for (vtkIdType i = 0; i < nNew; ++i)
  {
    outPD->CopyData(inPD, out.source_map[i], i);
  }

  // Cells: rebuilt from the kernel's int32 offsets/connectivity. With
  // remove_degenerate_cells=false the kernel keeps every cell 1:1 in input
  // order, so cell data passes through unchanged (matches stock PassData).
  vtkNew<vtkTypeInt32Array> offArr;
  offArr->SetNumberOfValues(static_cast<vtkIdType>(out.offsets.size()));
  std::memcpy(offArr->GetPointer(0), out.offsets.data(),
    sizeof(std::int32_t) * out.offsets.size());
  vtkNew<vtkTypeInt32Array> connArr;
  connArr->SetNumberOfValues(static_cast<vtkIdType>(out.conn.size()));
  if (!out.conn.empty())
  {
    std::memcpy(connArr->GetPointer(0), out.conn.data(),
      sizeof(std::int32_t) * out.conn.size());
  }
  vtkNew<vtkCellArray> outCells;
  outCells->SetData(offArr, connArr);

  // Cell types: cells are kept 1:1 in input order, so the output types EQUAL the
  // input types. Preserve the input's representation exactly -- stock
  // vtkStaticCleanUnstructuredGrid reuses the input type array, so a homogeneous
  // grid keeps its implicit (vtkConstantArray) types and a heterogeneous grid its
  // explicit per-cell array. Mirroring this avoids materializing an explicit type
  // array where stock keeps an implicit one (which would otherwise diverge).
  if (vtkUnsignedCharArray* inTypes = input->GetCellTypesArray())
  {
    output->SetCells(inTypes, outCells); // explicit per-cell types (1:1 with input)
  }
  else
  {
    // Implicit homogeneous types -> single constant type, same as the input.
    output->SetCells(nCells > 0 ? input->GetCellType(0) : VTK_EMPTY_CELL, outCells);
  }

  output->GetCellData()->PassData(input->GetCellData());
}
}

#endif // CVISTA_HAVE_OPENMP

namespace cvista
{
VTK_ABI_NAMESPACE_BEGIN

bool FastStaticCleanUnstructuredGrid(vtkUnstructuredGrid* input, vtkUnstructuredGrid* output,
  double effectiveTolerance, bool removeUnusedPoints, bool averagePointData, bool hasMergingArray,
  int outputPointsPrecision)
{
  if (!FastModeEnabled())
  {
    return false;
  }
#ifndef CVISTA_HAVE_OPENMP
  (void)input;
  (void)output;
  (void)effectiveTolerance;
  (void)removeUnusedPoints;
  (void)averagePointData;
  (void)hasMergingArray;
  (void)outputPointsPrecision;
  return false;
#else
  // Only the exact-merge default regime is accelerated; anything else (a real
  // tolerance sphere, point-data averaging, or merge-by-data-array) falls back.
  if (effectiveTolerance != 0.0 || averagePointData || hasMergingArray)
  {
    return false;
  }

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

  // Polyhedra need face connectivity the kernel doesn't rewrite -> fall back.
  for (vtkIdType i = 0; i < nCells; ++i)
  {
    if (ct[i] == VTK_POLYHEDRON)
    {
      return false;
    }
  }

  vtkDataArray* ptData = pts->GetData();
  const int ptType = ptData->GetDataType();
  if ((ptType != VTK_FLOAT && ptType != VTK_DOUBLE) || ptData->GetNumberOfComponents() != 3)
  {
    return false;
  }

  std::vector<std::int32_t> offBuf, connBuf;
  const std::int32_t* off32 = AsInt32(cells->GetOffsetsArray(), offBuf);
  const std::int32_t* conn32 = AsInt32(cells->GetConnectivityArray(), connBuf);

  if (ptType == VTK_DOUBLE)
  {
    const double* p = static_cast<const double*>(ptData->GetVoidPointer(0));
    pvu::clean::CleanInput<double, std::int32_t> in{ p, static_cast<size_t>(nPts), conn32, off32,
      ct, static_cast<std::int32_t>(nCells) };
    BuildOutput<double>(
      p, nPts, input, output, removeUnusedPoints, outputPointsPrecision, in, ptType);
  }
  else
  {
    const float* p = static_cast<const float*>(ptData->GetVoidPointer(0));
    pvu::clean::CleanInput<float, std::int32_t> in{ p, static_cast<size_t>(nPts), conn32, off32, ct,
      static_cast<std::int32_t>(nCells) };
    BuildOutput<float>(
      p, nPts, input, output, removeUnusedPoints, outputPointsPrecision, in, ptType);
  }
  return true;
#endif // CVISTA_HAVE_OPENMP
}

VTK_ABI_NAMESPACE_END
} // namespace cvista
