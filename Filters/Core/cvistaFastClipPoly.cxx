// SPDX-License-Identifier: BSD-3-Clause
// cvista opt-in fast polygonal clip: a threaded replacement for vtkClipPolyData's
// serial per-cell clip loop. Each cell's vtkCell::Clip is run on a thread-local
// output (its own points/locator/polys/point-data/cell-data), and the per-thread
// fragments are composited and then merged at coincident points to produce the
// single output the stock serial loop would have built.
//
// Built only under CVISTA_HAVE_OPENMP (mirroring the sibling fast adapters); the
// non-OpenMP stub returns false so Windows/non-OpenMP builds stay byte-exact via
// the stock fallback. The clip loop itself uses vtkSMPTools (not OpenMP) because
// it constructs VTK objects per thread; the OpenMP gate is kept purely for
// build-consistency with the rest of the EnableFast adapter stack.
#include "cvistaFastClipPoly.h"

#include "vtkCVISTASMPDefaults.h" // cvista::FastModeEnabled / RunFastFilterParallel

#ifdef CVISTA_HAVE_OPENMP

#include "vtkCellArray.h"
#include "vtkCellArrayIterator.h"
#include "vtkCellData.h"
#include "vtkDataArray.h"
#include "vtkFloatArray.h"
#include "vtkGenericCell.h"
#include "vtkMergePoints.h"
#include "vtkNew.h"
#include "vtkPointData.h"
#include "vtkPoints.h"
#include "vtkPolyData.h"
#include "vtkSMPThreadLocal.h"
#include "vtkSMPTools.h"
#include "vtkSmartPointer.h"

#include <vector>

namespace
{
//------------------------------------------------------------------------------
// Per-thread clip output. Each thread accumulates a SELF-CONTAINED clipped
// fragment: its own points, a vtkMergePoints over those points (so a thread
// dedupes coincident points WITHIN its own cell batch exactly as the stock
// single locator would within that batch), its own polys, and its own
// interpolated point data / per-poly cell data. Fragments are stitched in
// Reduce(); points coincident across thread boundaries are merged in the final
// pass (see ClipFunctor::FinalMerge below).
struct ClipThreadLocal
{
  vtkSmartPointer<vtkPoints> Points;
  vtkSmartPointer<vtkMergePoints> Locator;
  vtkSmartPointer<vtkCellArray> Polys;
  vtkSmartPointer<vtkPointData> OutPD;
  vtkSmartPointer<vtkCellData> OutCD;
  vtkSmartPointer<vtkGenericCell> Cell;
  vtkSmartPointer<vtkFloatArray> CellScalars;
  // Parallel to Points: the id of the cell that FIRST inserted each thread-local
  // point. Lets the global merge reproduce stock's "first cell to produce a
  // coincident point wins" rule (lowest cell id), so interpolated point data at
  // coincident/on-plane points is byte-identical to the serial filter.
  std::vector<vtkIdType> PointCellId;
  bool Initialized = false;
};

//------------------------------------------------------------------------------
// Threaded per-cell clip functor for a polys-only vtkPolyData. Every cell is
// dimension 2, so vtkCell::Clip emits into the thread-local "polys" cell array
// (the dimension-2 branch of the stock loop) and there is no Verts/Lines
// routing to reproduce.
struct ClipFunctor
{
  vtkPolyData* Input;
  vtkDataArray* ClipScalars;
  double Value;
  int InsideOut;
  vtkPointData* InPD;
  vtkCellData* InCD;
  bool CopyScalars;
  int PointsDataType; // output point precision (= input point type by default)

  vtkSMPThreadLocal<ClipThreadLocal> LocalData;

  // Allocate this thread's self-contained output the first time it runs. (Done
  // lazily in operator() rather than Initialize() so we are robust to either
  // call convention; the Initialized flag makes it idempotent.)
  ClipThreadLocal& SetupLocal()
  {
    ClipThreadLocal& tl = this->LocalData.Local();
    if (tl.Initialized)
    {
      return tl;
    }
    tl.Points = vtkSmartPointer<vtkPoints>::New();
    tl.Points->SetDataType(this->PointsDataType);
    tl.Locator = vtkSmartPointer<vtkMergePoints>::New();
    // Insert over the FULL input bounds so every thread's locator bins points on
    // the same grid -- coincident points land in the same relative bucket,
    // matching stock's single-locator behaviour within a batch.
    tl.Locator->InitPointInsertion(tl.Points, this->Input->GetBounds());
    tl.Polys = vtkSmartPointer<vtkCellArray>::New();
    tl.OutPD = vtkSmartPointer<vtkPointData>::New();
    // Mirror the caller's CopyScalars On/Off decision so interpolated point data
    // is allocated array-for-array identically to the stock outPD.
    tl.OutPD->CopyAllOn();
    tl.OutPD->SetCopyScalars(this->CopyScalars);
    tl.OutPD->InterpolateAllocate(this->InPD);
    tl.OutCD = vtkSmartPointer<vtkCellData>::New();
    tl.OutCD->CopyAllocate(this->InCD);
    tl.Cell = vtkSmartPointer<vtkGenericCell>::New();
    tl.CellScalars = vtkSmartPointer<vtkFloatArray>::New();
    tl.CellScalars->Allocate(VTK_CELL_SIZE);
    tl.Initialized = true;
    return tl;
  }

  void Initialize() { this->SetupLocal(); }

  void operator()(vtkIdType begin, vtkIdType end)
  {
    ClipThreadLocal& tl = this->SetupLocal();
    vtkGenericCell* cell = tl.Cell;
    vtkFloatArray* cellScalars = tl.CellScalars;
    vtkMergePoints* locator = tl.Locator;
    vtkCellArray* polys = tl.Polys;
    vtkPointData* outPD = tl.OutPD;
    vtkCellData* outCD = tl.OutCD;

    for (vtkIdType cellId = begin; cellId < end; ++cellId)
    {
      // input->GetCell(cellId, cell) fills the generic cell with this cell's
      // points + ids (the same data the stock loop reads, in the same order).
      this->Input->GetCell(cellId, cell);
      const vtkIdType numberOfPoints = cell->GetNumberOfPoints();

      // Gather this cell's clip scalars from the per-input-point array by point
      // id -- byte-identical to the stock loop's cellScalars assembly.
      for (vtkIdType i = 0; i < numberOfPoints; ++i)
      {
        const double s = this->ClipScalars->GetComponent(cell->GetPointId(i), 0);
        cellScalars->InsertTuple(i, &s);
      }

      // Clip into this thread's self-contained output. Passing the real cellId
      // makes outCD->CopyData(inCD, cellId, ...) source cell data from the
      // correct input cell, exactly as stock does. Polys-only input => every
      // cell is dimension 2 => connectivity target is `polys` (newPolys).
      const vtkIdType nBefore = tl.Points->GetNumberOfPoints();
      cell->Clip(this->Value, cellScalars, locator, polys, this->InPD, outPD, this->InCD, cellId,
        outCD, this->InsideOut);
      // Tag the points this cell newly inserted (points already present from an
      // earlier, lower-id cell in this thread were deduped by the locator and
      // keep their earlier tag -- so each point's tag is the lowest cell id that
      // produced it within this thread).
      const vtkIdType nAfter = tl.Points->GetNumberOfPoints();
      if (nAfter > nBefore)
      {
        tl.PointCellId.resize(static_cast<size_t>(nAfter));
        for (vtkIdType q = nBefore; q < nAfter; ++q)
        {
          tl.PointCellId[static_cast<size_t>(q)] = cellId;
        }
      }
    }
  }

  // No Reduce() here: compositing + the cross-thread coincident merge are done
  // serially in FinalMerge() after the For completes, so the (process-global,
  // not-thread-safe) merge runs outside any parallel scope.
  void Reduce() {}
};
} // anonymous namespace

namespace cvista
{
VTK_ABI_NAMESPACE_BEGIN

bool FastClipPolyData(vtkPolyData* input, vtkPolyData* output, vtkDataArray* clipScalars,
  double value, int insideOut, vtkPointData* inPD, vtkCellData* inCD, bool copyScalars)
{
  if (!FastModeEnabled())
  {
    return false;
  }
  // ---- Engagement gate -------------------------------------------------------
  // Polys-only surface clip with at least one cell. Verts/lines/strips would
  // each route to their own (dimension 0/1) connectivity arrays which this path
  // does not assemble, so bail to the stock loop when any are present.
  if (input == nullptr || clipScalars == nullptr || inPD == nullptr || inCD == nullptr)
  {
    return false;
  }
  const vtkIdType numCells = input->GetNumberOfCells();
  if (numCells < 1)
  {
    return false;
  }
  if (input->GetNumberOfVerts() != 0 || input->GetNumberOfLines() != 0 ||
    input->GetNumberOfStrips() != 0 || input->GetNumberOfPolys() != numCells)
  {
    return false;
  }
  vtkPoints* inPts = input->GetPoints();
  if (inPts == nullptr)
  {
    return false;
  }

  // Force the cell-map build NOW (single-threaded): vtkPolyData::GetCell lazily
  // builds it on first use, which would race across the worker threads below.
  input->BuildCells();

  // ---- Threaded per-cell clip ------------------------------------------------
  ClipFunctor functor;
  functor.Input = input;
  functor.ClipScalars = clipScalars;
  functor.Value = value;
  functor.InsideOut = insideOut;
  functor.InPD = inPD;
  functor.InCD = inCD;
  functor.CopyScalars = copyScalars;
  functor.PointsDataType = inPts->GetDataType();

  // RunFastFilterParallel applies the EnableFast threading config (STDThread,
  // honoring VTK_SMP_MAX_THREADS / Initialize(n) overrides) and inherits an
  // outer parallel scope if one is active. vtkSMPTools::For invokes the
  // functor's Initialize()/operator()/Reduce().
  // Run the threaded per-cell clip AND composite the thread-local fragments in
  // ONE parallel scope. vtkSMPThreadLocal storage is keyed to the active SMP
  // backend, so functor.LocalData is only iterable while we remain inside the
  // scope that RunFastFilterParallel -> LocalScope established for the For;
  // iterating it AFTER RunFastFilterParallel returns (backend restored) yields
  // zero entries (and reads freed storage under multiple threads).
  bool produced = false;
  cvista::RunFastFilterParallel([&]() {
    vtkSMPTools::For(0, numCells, functor);
    produced = true;

  // ---- Composite the per-thread fragments ------------------------------------
  // The output is POINTS-relaxed: we concatenate the thread fragments (so output
  // point order follows thread/batch order, not stock serial-insertion order),
  // then dedupe points coincident ACROSS thread boundaries in a single serial
  // vtkMergePoints pass below. The point SET and the poly multiset are identical
  // to stock; only the renumbering differs.
  //
  // NOTE ON THE MERGE STEP: we deliberately use a serial vtkMergePoints pass
  // (not the parallel pvaClean kernel that cvistaFastCleanPoly uses) for the
  // cross-thread dedupe. It is correct and self-contained: clip-generated
  // triangles never collapse here (so there is no degenerate-cell case to fall
  // back on, which pvaClean would force), and InsertUniquePoint reproduces stock
  // exact-coincidence merging. The big parallel win is the per-cell Clip above;
  // the merge is a cheap coordinate-insert + remap. If this becomes a measured
  // bottleneck, swap in the parallel kernel here.

  // Final merged output, built with a single serial locator over all composited
  // points. Mirrors the stock output point precision (input point type).
  vtkNew<vtkPoints> newPoints;
  newPoints->SetDataType(inPts->GetDataType());
  vtkNew<vtkMergePoints> mergeLocator;
  mergeLocator->InitPointInsertion(newPoints, input->GetBounds());

  // Output point data: allocate identically to a thread-local OutPD so we can
  // CopyData merged points from the per-thread interpolated arrays.
  vtkPointData* outPD = output->GetPointData();
  outPD->CopyAllOn();
  outPD->SetCopyScalars(copyScalars);
  outPD->InterpolateAllocate(inPD);

  // Output polys + cell data, sized 1:1 with the union of thread polys (every
  // thread poly survives; only its point ids are remapped through the merge).
  vtkNew<vtkCellArray> newPolys;
  vtkCellData* outCD = output->GetCellData();
  outCD->CopyAllocate(inCD);

  double x[3];
  std::vector<vtkIdType> idMap; // composited-point id -> merged output point id
  // Per merged output point: the lowest cell id that has contributed it so far.
  // Stock keeps the FIRST (lowest-cell-id) contributor's interpolated data at a
  // coincident point; we reproduce that by overwriting an existing point's data
  // only when this fragment's contributor has a strictly lower cell id. The
  // explicit cell-id compare makes the result independent of thread/fragment
  // iteration order -> byte-identical point data vs the serial filter.
  std::vector<vtkIdType> outPointCellId;
  vtkIdType outCellId = 0;
  for (auto& tl : functor.LocalData)
  {
    if (!tl.Initialized)
    {
      continue; // a thread that never ran (no cells in its range)
    }
    vtkPoints* tlPts = tl.Points;
    const vtkIdType nLocal = tlPts->GetNumberOfPoints();
    idMap.assign(static_cast<size_t>(nLocal), -1);

    // Merge this fragment's points into the global locator, recording the
    // composited->output id remap and keeping the lowest-cell-id data.
    for (vtkIdType p = 0; p < nLocal; ++p)
    {
      tlPts->GetPoint(p, x);
      const vtkIdType cid = tl.PointCellId[static_cast<size_t>(p)];
      vtkIdType outId;
      if (mergeLocator->InsertUniquePoint(x, outId))
      {
        // First time this coordinate is seen: copy interpolated point data from
        // this fragment's matching point and record its contributing cell id.
        outPD->CopyData(tl.OutPD, p, outId);
        if (static_cast<size_t>(outId) >= outPointCellId.size())
        {
          outPointCellId.resize(static_cast<size_t>(outId) + 1);
        }
        outPointCellId[static_cast<size_t>(outId)] = cid;
      }
      else if (cid < outPointCellId[static_cast<size_t>(outId)])
      {
        // A lower-cell-id contributor for an existing coincident point: take its
        // data (matching stock's first-cell-wins), keep the (bit-identical) coords.
        outPD->CopyData(tl.OutPD, p, outId);
        outPointCellId[static_cast<size_t>(outId)] = cid;
      }
      idMap[static_cast<size_t>(p)] = outId;
    }

    // Append this fragment's polys with point ids remapped, copying cell data
    // 1:1 (thread OutCD already holds one tuple per emitted poly, sourced from
    // the correct input cell).
    vtkCellArray* tlPolys = tl.Polys;
    const vtkIdType nPolys = tlPolys->GetNumberOfCells();
    vtkIdType npts;
    const vtkIdType* pts;
    auto iter = vtk::TakeSmartPointer(tlPolys->NewIterator());
    vtkIdType localPoly = 0;
    for (iter->GoToFirstCell(); !iter->IsDoneWithTraversal(); iter->GoToNextCell(), ++localPoly)
    {
      iter->GetCurrentCell(npts, pts);
      newPolys->InsertNextCell(npts);
      for (vtkIdType k = 0; k < npts; ++k)
      {
        newPolys->InsertCellPoint(idMap[static_cast<size_t>(pts[k])]);
      }
      outCD->CopyData(tl.OutCD, localPoly, outCellId);
      ++outCellId;
    }
    (void)nPolys;
  }

  output->SetPoints(newPoints);
  if (newPolys->GetNumberOfCells() > 0)
  {
    output->SetPolys(newPolys);
  }
  }); // end RunFastFilterParallel scope (thread-locals consumed above)

  if (!produced)
  {
    return false; // fast scope never ran the For; let the caller use stock
  }
  output->Squeeze();
  return true;
}

VTK_ABI_NAMESPACE_END
} // namespace cvista

#else // !CVISTA_HAVE_OPENMP -- bit-exact stub, caller runs the stock serial loop.

namespace cvista
{
VTK_ABI_NAMESPACE_BEGIN

bool FastClipPolyData(
  vtkPolyData*, vtkPolyData*, vtkDataArray*, double, int, vtkPointData*, vtkCellData*, bool)
{
  return false;
}

VTK_ABI_NAMESPACE_END
} // namespace cvista

#endif // CVISTA_HAVE_OPENMP
