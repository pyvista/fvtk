// SPDX-License-Identifier: BSD-3-Clause
// fvtk opt-in fast coincident-point merge for polygonal data: VTK adapter over
// the vendored pyvista-algorithms OpenMP point-dedup kernel (pvaClean.h, MIT).
// This TU is built with OpenMP and excluded from the unity build so the vendored
// code stays isolated.
#include "fvtkFastCleanPoly.h"

#include "vtkFVTKSMPDefaults.h" // fvtk::FastModeEnabled (needed even without OpenMP)

#ifdef FVTK_HAVE_OPENMP

#include "vtkAlgorithm.h" // SINGLE_PRECISION / DOUBLE_PRECISION
#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkCellType.h"
#include "vtkDataArray.h"
#include "vtkIdTypeArray.h"
#include "vtkNew.h"
#include "vtkPointData.h"
#include "vtkPoints.h"
#include "vtkPolyData.h"
#include "vtkTypeInt32Array.h"
#include "vtkTypeInt64Array.h"
#include "vtkUnstructuredGrid.h"

#include "pvaClean.h" // vendored MIT kernel (namespace pvu::clean); pulls in <omp.h>

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

// Build the merged output polydata from a run_clean result of point type T.
template <typename T>
void BuildOutput(vtkPolyData* input, vtkPolyData* output, int outputPointsPrecision,
  int inPtType, pvu::clean::CleanOutput<T>& out)
{
  const vtkIdType nNew = static_cast<vtkIdType>(out.points.size() / 3);
  const vtkIdType nPolys = static_cast<vtkIdType>(out.offsets.size()) - 1;

  int outType = inPtType;
  if (outputPointsPrecision == vtkAlgorithm::SINGLE_PRECISION)
  {
    outType = VTK_FLOAT;
  }
  else if (outputPointsPrecision == vtkAlgorithm::DOUBLE_PRECISION)
  {
    outType = VTK_DOUBLE;
  }
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

  // Polys: rebuilt from the kernel's int32 offsets/connectivity. With
  // remove_degenerate_cells=false the kernel keeps every poly 1:1 in input order.
  vtkNew<vtkTypeInt32Array> offArr;
  offArr->SetNumberOfValues(static_cast<vtkIdType>(out.offsets.size()));
  std::memcpy(
    offArr->GetPointer(0), out.offsets.data(), sizeof(std::int32_t) * out.offsets.size());
  vtkNew<vtkTypeInt32Array> connArr;
  connArr->SetNumberOfValues(static_cast<vtkIdType>(out.conn.size()));
  if (!out.conn.empty())
  {
    std::memcpy(connArr->GetPointer(0), out.conn.data(), sizeof(std::int32_t) * out.conn.size());
  }
  vtkNew<vtkCellArray> polys;
  polys->SetData(offArr, connArr);
  output->SetPolys(polys);

  // Cell data: polys kept 1:1 in input order (input is polys-only, so the input
  // cell ids ARE the poly ids) -> pass cell data through unchanged.
  (void)nPolys;
  output->GetCellData()->PassData(input->GetCellData());
}
}

#endif // FVTK_HAVE_OPENMP

namespace fvtk
{
VTK_ABI_NAMESPACE_BEGIN

bool FastCleanPolyData(vtkPolyData* input, vtkPolyData* output, bool pointMerging,
  double effectiveTolerance, int outputPointsPrecision)
{
  if (!FastModeEnabled())
  {
    return false;
  }
#ifndef FVTK_HAVE_OPENMP
  (void)input;
  (void)output;
  (void)pointMerging;
  (void)effectiveTolerance;
  (void)outputPointsPrecision;
  return false;
#else
  // Exact-merge default regime, restricted to the common polys-only surface case.
  if (!pointMerging || effectiveTolerance != 0.0)
  {
    return false;
  }
  // Polys only -- verts/lines/strips would each need their own (degenerate-aware)
  // handling that vtkCleanPolyData performs; fall back when present.
  if (input->GetNumberOfPolys() == 0 || input->GetNumberOfVerts() != 0 ||
    input->GetNumberOfLines() != 0 || input->GetNumberOfStrips() != 0)
  {
    return false;
  }
  // Global ids change the merge predicate; ghost points change point-data copy.
  if (input->GetPointData()->GetGlobalIds() != nullptr || input->HasAnyGhostPoints())
  {
    return false;
  }

  vtkPoints* pts = input->GetPoints();
  vtkCellArray* polys = input->GetPolys();
  if (!pts || !pts->GetData() || !polys)
  {
    return false;
  }
  const vtkIdType nPolys = input->GetNumberOfPolys();
  const vtkIdType nPts = input->GetNumberOfPoints();
  if (nPolys == 0 || nPts == 0 || nPts > static_cast<vtkIdType>(0x7FFFFFFF))
  {
    return false;
  }

  vtkDataArray* ptData = pts->GetData();
  const int ptType = ptData->GetDataType();
  if ((ptType != VTK_FLOAT && ptType != VTK_DOUBLE) || ptData->GetNumberOfComponents() != 3)
  {
    return false;
  }

  std::vector<std::int32_t> offBuf, connBuf;
  const std::int32_t* off32 = AsInt32(polys->GetOffsetsArray(), offBuf);
  const std::int32_t* conn32 = AsInt32(polys->GetConnectivityArray(), connBuf);

  // Per-poly cell type (by size). The kernel only consults cell_types for
  // polyhedron detection and degenerate conversion (disabled here); the merge
  // and the connectivity rewrite are driven by offsets, so this is purely to
  // satisfy the kernel's input contract with sane VTK type tags.
  std::vector<unsigned char> typeBuf(static_cast<size_t>(nPolys));
  for (vtkIdType c = 0; c < nPolys; ++c)
  {
    const std::int32_t w = off32[c + 1] - off32[c];
    typeBuf[static_cast<size_t>(c)] =
      (w == 3 ? VTK_TRIANGLE : (w == 4 ? VTK_QUAD : VTK_POLYGON));
  }

  pvu::clean::CleanOptions opts;
  opts.tolerance = 0.0;
  opts.absolute = false;
  opts.remove_unused_points = true;
  opts.remove_degenerate_cells = false; // keep polys 1:1; detect collapse below
  opts.drop_nonfinite_points = false;

  bool degenerate = false;
  if (ptType == VTK_DOUBLE)
  {
    const double* p = static_cast<const double*>(ptData->GetVoidPointer(0));
    pvu::clean::CleanInput<double, std::int32_t> in{ p, static_cast<size_t>(nPts), conn32, off32,
      typeBuf.data(), static_cast<std::int32_t>(nPolys) };
    pvu::clean::CleanOutput<double> out;
    pvu::clean::run_clean(in, opts, out, 0);
    // A poly collapses iff two of its corners merged -> a repeated id. stock
    // vtkCleanPolyData converts such a poly to a line/vertex, which this path does
    // not reproduce, so bail and let the standard path handle it.
    for (vtkIdType c = 0; c < nPolys && !degenerate; ++c)
    {
      for (std::int32_t a = out.offsets[c]; a < out.offsets[c + 1] && !degenerate; ++a)
      {
        for (std::int32_t b = a + 1; b < out.offsets[c + 1]; ++b)
        {
          if (out.conn[a] == out.conn[b])
          {
            degenerate = true;
            break;
          }
        }
      }
    }
    if (degenerate || out.needs_polyhedron_fallback)
    {
      return false;
    }
    BuildOutput<double>(input, output, outputPointsPrecision, ptType, out);
  }
  else
  {
    const float* p = static_cast<const float*>(ptData->GetVoidPointer(0));
    pvu::clean::CleanInput<float, std::int32_t> in{ p, static_cast<size_t>(nPts), conn32, off32,
      typeBuf.data(), static_cast<std::int32_t>(nPolys) };
    pvu::clean::CleanOutput<float> out;
    pvu::clean::run_clean(in, opts, out, 0);
    for (vtkIdType c = 0; c < nPolys && !degenerate; ++c)
    {
      for (std::int32_t a = out.offsets[c]; a < out.offsets[c + 1] && !degenerate; ++a)
      {
        for (std::int32_t b = a + 1; b < out.offsets[c + 1]; ++b)
        {
          if (out.conn[a] == out.conn[b])
          {
            degenerate = true;
            break;
          }
        }
      }
    }
    if (degenerate || out.needs_polyhedron_fallback)
    {
      return false;
    }
    BuildOutput<float>(input, output, outputPointsPrecision, ptType, out);
  }
  return true;
#endif // FVTK_HAVE_OPENMP
}

VTK_ABI_NAMESPACE_END
} // namespace fvtk
