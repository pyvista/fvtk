// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkClipPolyData.h"

#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkExecutive.h"
#include "vtkFloatArray.h"
#include "vtkGenericCell.h"
#include "vtkIdList.h"
#include "vtkImplicitFunction.h"
#include "vtkIncrementalPointLocator.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkLine.h"
#include "vtkMergePoints.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkPoints.h"
#include "vtkPolyData.h"
#include "vtkTriangle.h"
#include "vtkTypeInt64Array.h"

#include "cvistaFastClipPoly.h" // cvista opt-in parallel polys-only clip (EnableFast)

#include <cmath>

VTK_ABI_NAMESPACE_BEGIN
vtkStandardNewMacro(vtkClipPolyData);
vtkCxxSetObjectMacro(vtkClipPolyData, ClipFunction, vtkImplicitFunction);

namespace
{
//------------------------------------------------------------------------------
// One vtkPolyData cell-array target (Verts/Lines/Polys/Strips) resolved to raw
// typed pointers for the dominant Int64-AOS storage (vtkCellArray's default), so
// the per-cell clip loop can read a cell's point ids inline instead of calling
// the out-of-line, cross-shared-library vtkPolyData::GetCell -> vtkCellArray::
// GetCellAtId once per cell (which also re-runs vtkCellArray's StorageType switch
// every cell). The inline reads are byte-identical to GetCellAtId's vtkIdList
// copy (numberOfPoints = offsets[local+1]-offsets[local]; ids copied from
// connectivity), in the same order; no FP. When Conn64 is null (non-Int64
// storage, or no cells in this target) the caller falls back to GetCellAtId.
struct ClipCellArrayView
{
  vtkCellArray* Array = nullptr;       // the target cell array (never null when used)
  const vtkTypeInt64* Offsets64 = nullptr;
  const vtkTypeInt64* Conn64 = nullptr;
  vtkIdType NumberOfCells = 0;         // cells in this target
  vtkIdType GlobalBegin = 0;           // global cellId of this target's first cell

  void Initialize(vtkCellArray* array, vtkIdType globalBegin)
  {
    this->Array = array;
    this->GlobalBegin = globalBegin;
    this->NumberOfCells = array ? array->GetNumberOfCells() : 0;
    this->Offsets64 = nullptr;
    this->Conn64 = nullptr;
    if (array && array->GetStorageType() == vtkCellArray::StorageTypes::Int64)
    {
      if (auto* offs = array->GetOffsetsArray64())
      {
        if (auto* cn = array->GetConnectivityArray64())
        {
          this->Offsets64 = offs->GetPointer(0);
          this->Conn64 = cn->GetPointer(0);
        }
      }
    }
  }
};
} // anonymous namespace

//------------------------------------------------------------------------------
// Construct with user-specified implicit function; InsideOut turned off; value
// set to 0.0; and generate clip scalars turned off.
vtkClipPolyData::vtkClipPolyData(vtkImplicitFunction* cf)
{
  this->ClipFunction = cf;
  this->InsideOut = 0;
  this->Locator = nullptr;
  this->Value = 0.0;
  this->GenerateClipScalars = 0;
  this->GenerateClippedOutput = 0;
  this->OutputPointsPrecision = DEFAULT_PRECISION;

  this->SetNumberOfOutputPorts(2);

  vtkPolyData* output2 = vtkPolyData::New();
  this->GetExecutive()->SetOutputData(1, output2);
  output2->Delete();
}

//------------------------------------------------------------------------------
vtkClipPolyData::~vtkClipPolyData()
{
  if (this->Locator)
  {
    this->Locator->UnRegister(this);
    this->Locator = nullptr;
  }
  this->SetClipFunction(nullptr);
}

//------------------------------------------------------------------------------
// Overload standard modified time function. If Clip functions is modified,
// then this object is modified as well.
vtkMTimeType vtkClipPolyData::GetMTime()
{
  vtkMTimeType mTime = this->Superclass::GetMTime();
  vtkMTimeType time;

  if (this->ClipFunction != nullptr)
  {
    time = this->ClipFunction->GetMTime();
    mTime = (time > mTime ? time : mTime);
  }
  if (this->Locator != nullptr)
  {
    time = this->Locator->GetMTime();
    mTime = (time > mTime ? time : mTime);
  }

  return mTime;
}

vtkPolyData* vtkClipPolyData::GetClippedOutput()
{
  return vtkPolyData::SafeDownCast(this->GetExecutive()->GetOutputData(1));
}

//------------------------------------------------------------------------------
//
// Clip through data generating surface.
//
int vtkClipPolyData::RequestData(vtkInformation* vtkNotUsed(request),
  vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  // get the info objects
  vtkInformation* inInfo = inputVector[0]->GetInformationObject(0);
  vtkInformation* outInfo = outputVector->GetInformationObject(0);

  // get the input and output
  vtkPolyData* input = vtkPolyData::SafeDownCast(inInfo->Get(vtkDataObject::DATA_OBJECT()));
  vtkPolyData* output = vtkPolyData::SafeDownCast(outInfo->Get(vtkDataObject::DATA_OBJECT()));

  vtkIdType cellId, i, updateTime;
  vtkPoints* cellPts;
  vtkDataArray* clipScalars;
  vtkFloatArray* cellScalars;
  vtkGenericCell* cell;
  vtkCellArray *newVerts, *newLines, *newPolys, *connList = nullptr;
  vtkCellArray *clippedVerts = nullptr, *clippedLines = nullptr;
  vtkCellArray *clippedPolys = nullptr, *clippedList = nullptr;
  vtkPoints* newPoints;
  vtkIdList* cellIds;
  double s;
  vtkIdType estimatedSize, numCells = input->GetNumberOfCells();
  vtkIdType numPts = input->GetNumberOfPoints();
  vtkPoints* inPts = input->GetPoints();
  int numberOfPoints;
  vtkPointData *inPD = input->GetPointData(), *outPD = output->GetPointData();
  vtkCellData *inCD = input->GetCellData(), *outCD = output->GetCellData();
  vtkCellData* outClippedCD = nullptr;

  vtkDebugMacro(<< "Clipping polygonal data");

  // Initialize self; create output objects
  //
  if (numPts < 1 || inPts == nullptr)
  {
    vtkDebugMacro(<< "No data to clip");
    return 1;
  }

  if (!this->ClipFunction && this->GenerateClipScalars)
  {
    vtkErrorMacro(<< "Cannot generate clip scalars if no clip function defined");
    return 1;
  }

  // Determine whether we're clipping with input scalars or a clip function
  // and to necessary setup.
  if (this->ClipFunction)
  {
    vtkFloatArray* tmpScalars = vtkFloatArray::New();
    tmpScalars->SetNumberOfTuples(numPts);
    inPD = vtkPointData::New();
    inPD->ShallowCopy(input->GetPointData()); // copies original
    if (this->GenerateClipScalars)
    {
      inPD->SetScalars(tmpScalars);
    }
    for (i = 0; i < numPts; i++)
    {
      s = this->ClipFunction->FunctionValue(inPts->GetPoint(i));
      tmpScalars->SetComponent(i, 0, s);
    }
    clipScalars = tmpScalars;
  }
  else // using input scalars
  {
    clipScalars = inPD->GetScalars();
    if (!clipScalars)
    {
      vtkErrorMacro(<< "Cannot clip without clip function or input scalars");
      return 1;
    }
  }

  // Create objects to hold output of clip operation
  //
  estimatedSize = numCells;
  estimatedSize = estimatedSize / 1024 * 1024; // multiple of 1024
  estimatedSize = std::max<vtkIdType>(estimatedSize, 1024);

  newPoints = vtkPoints::New();

  // Set the desired precision for the points in the output.
  if (this->OutputPointsPrecision == vtkAlgorithm::DEFAULT_PRECISION)
  {
    newPoints->SetDataType(input->GetPoints()->GetDataType());
  }
  else if (this->OutputPointsPrecision == vtkAlgorithm::SINGLE_PRECISION)
  {
    newPoints->SetDataType(VTK_FLOAT);
  }
  else if (this->OutputPointsPrecision == vtkAlgorithm::DOUBLE_PRECISION)
  {
    newPoints->SetDataType(VTK_DOUBLE);
  }

  newPoints->Allocate(numPts, numPts / 2);
  newVerts = vtkCellArray::New();
  newVerts->AllocateEstimate(estimatedSize, 1);
  newLines = vtkCellArray::New();
  newLines->AllocateEstimate(estimatedSize, 2);
  newPolys = vtkCellArray::New();
  newPolys->AllocateEstimate(estimatedSize, 4);

  // locator used to merge potentially duplicate points
  if (this->Locator == nullptr)
  {
    this->CreateDefaultLocator();
  }
  this->Locator->InitPointInsertion(newPoints, input->GetBounds());

  if (!this->GenerateClipScalars && !input->GetPointData()->GetScalars())
  {
    outPD->CopyScalarsOff();
  }
  else
  {
    outPD->CopyScalarsOn();
  }
  outPD->InterpolateAllocate(inPD, estimatedSize, estimatedSize / 2);
  outCD->CopyAllocate(inCD, estimatedSize, estimatedSize / 2);

  // If generating second output, setup clipped output
  if (this->GenerateClippedOutput)
  {
    this->GetClippedOutput()->Initialize();
    outClippedCD = this->GetClippedOutput()->GetCellData();
    outClippedCD->CopyAllocate(inCD, estimatedSize, estimatedSize / 2);
    clippedVerts = vtkCellArray::New();
    clippedVerts->AllocateEstimate(estimatedSize, 1);
    clippedLines = vtkCellArray::New();
    clippedLines->AllocateEstimate(estimatedSize, 2);
    clippedPolys = vtkCellArray::New();
    clippedPolys->AllocateEstimate(estimatedSize, 4);
  }

  cellScalars = vtkFloatArray::New();
  cellScalars->Allocate(VTK_CELL_SIZE);

  // cvista opt-in parallel fast path (env CVISTA_FAST / cvista.EnableFast()). Engages
  // only for the single-output, polys-only surface-clip regime; replaces the
  // serial per-cell Clip loop + shared-locator merge below with a threaded
  // per-cell clip and a coincident-point merge. POINTS-relaxed (same point set
  // and polys, different point numbering). When it declines (fast mode off, or
  // any unsupported feature -- second clipped output, verts/lines/strips) it
  // returns false and the byte-for-byte stock path below runs unchanged.
  //
  // outPD CopyScalars mirrors the On/Off decision made above; the fast path
  // allocates its interpolated point data the same way.
  const bool copyScalars = (this->GenerateClipScalars != 0) ||
    (input->GetPointData()->GetScalars() != nullptr);
  if (!this->GenerateClippedOutput &&
    cvista::FastClipPolyData(
      input, output, clipScalars, this->Value, this->InsideOut, inPD, inCD, copyScalars))
  {
    // The fast path filled `output` (points + polys + interpolated point data +
    // passed-through cell data). Release everything the stock path allocated and
    // return, exactly as the stock path would have on completion.
    if (this->ClipFunction)
    {
      clipScalars->Delete(); // the tmpScalars vtkFloatArray::New() above
      inPD->Delete();        // the shallow-copied vtkPointData::New() above
    }
    newVerts->Delete();
    newLines->Delete();
    newPolys->Delete();
    newPoints->Delete();
    cellScalars->Delete();
    this->Locator->Initialize(); // release locator memory, as the stock tail does
    return 1;
  }

  // Resolve the four vtkPolyData cell-array targets to raw Int64-AOS pointers so
  // the per-cell loop below can read each cell's point ids inline instead of
  // calling the virtual, cross-shared-library vtkPolyData::GetCell(cellId, cell)
  // once per cell. vtkPolyData lays out global cell ids in the order
  // Verts, Lines, Polys, Strips (see vtkPolyData::BuildCells), with the local id
  // within a target equal to (globalCellId - target's GlobalBegin); we reproduce
  // exactly that mapping. The inline ids/points read below are byte-identical to
  // GetCell's GetCellAtId + vtkPoints::GetPoints, in the same order; no FP.
  ClipCellArrayView clipViews[4];
  {
    vtkCellArray* targets[4] = { input->GetVerts(), input->GetLines(), input->GetPolys(),
      input->GetStrips() };
    vtkIdType globalBegin = 0;
    for (int t = 0; t < 4; ++t)
    {
      clipViews[t].Initialize(targets[t], globalBegin);
      globalBegin += clipViews[t].NumberOfCells;
    }
  }
  // Cursor into clipViews: clipViews[viewIdx] is the target owning the current
  // cellId, and viewEnd is its one-past-the-last global cellId. Advancing the
  // cursor in lockstep with cellId avoids a per-cell search for the owning array.
  int viewIdx = 0;
  vtkIdType viewEnd = clipViews[0].NumberOfCells;

  // perform clipping on cells
  bool abort = false;
  updateTime = numCells / 20 + 1; // update roughly every 5%
  cell = vtkGenericCell::New();
  for (cellId = 0; cellId < numCells && !abort; cellId++)
  {
    // Advance to the cell-array target that owns this global cellId (skipping
    // any empty targets). Matches vtkPolyData's Verts/Lines/Polys/Strips order.
    while (viewIdx < 3 && cellId >= viewEnd)
    {
      ++viewIdx;
      viewEnd += clipViews[viewIdx].NumberOfCells;
    }
    const ClipCellArrayView& view = clipViews[viewIdx];

    // Devirtualized equivalent of input->GetCell(cellId, cell). GetCellType()
    // reads the cell-map tag inline (no cross-library call, no StorageType
    // switch) and, exactly like GetCell's switch, returns VTK_EMPTY_CELL for
    // deleted/unsupported cells -- in which case GetCell sets the generic cell to
    // an empty cell and leaves its Points/PointIds untouched (stale). We
    // reproduce that here, then fill the ids/points inline for the live case.
    const int cellType = input->GetCellType(cellId);
    bool live;
    switch (cellType)
    {
      case VTK_VERTEX:
      case VTK_POLY_VERTEX:
      case VTK_LINE:
      case VTK_POLY_LINE:
      case VTK_TRIANGLE:
      case VTK_QUAD:
      case VTK_POLYGON:
      case VTK_TRIANGLE_STRIP:
        cell->SetCellType(cellType);
        live = true;
        break;
      default:
        cell->SetCellTypeToEmptyCell();
        live = false;
        break;
    }
    if (live)
    {
      vtkIdList* cellPtIds = cell->GetPointIds();
      const vtkIdType localId = cellId - view.GlobalBegin;
      if (view.Conn64)
      {
        // Inline the zero-copy Int64-AOS GetCellAtId path (byte-identical to
        // vtkCellArray::GetCellAtId(localId, vtkIdList*) for Int64 storage):
        // numberOfPoints = offsets[localId+1]-offsets[localId]; ids copied from
        // connectivity. Avoids the per-cell cross-library call + StorageType
        // switch.
        const vtkTypeInt64 begin = view.Offsets64[localId];
        const vtkIdType n = static_cast<vtkIdType>(view.Offsets64[localId + 1] - begin);
        cellPtIds->SetNumberOfIds(n);
        vtkIdType* idPtr = cellPtIds->GetPointer(0);
        const vtkTypeInt64* connPtr = view.Conn64 + begin;
        for (vtkIdType k = 0; k < n; ++k)
        {
          idPtr[k] = static_cast<vtkIdType>(connPtr[k]);
        }
      }
      else
      {
        // Non-Int64 storage: fall back to the same call GetCell makes.
        view.Array->GetCellAtId(localId, cellPtIds);
      }
      inPts->GetPoints(cellPtIds, cell->GetPoints());
    }
    cellPts = cell->GetPoints();
    cellIds = cell->GetPointIds();
    numberOfPoints = cellPts->GetNumberOfPoints();

    // evaluate implicit cutting function
    for (i = 0; i < numberOfPoints; i++)
    {
      s = clipScalars->GetComponent(cellIds->GetId(i), 0);
      cellScalars->InsertTuple(i, &s);
    }

    switch (cell->GetCellDimension())
    {
      case 0: // points are generated-------------------------------
        connList = newVerts;
        clippedList = clippedVerts;
        break;

      case 1: // lines are generated----------------------------------
        connList = newLines;
        clippedList = clippedLines;
        break;

      case 2: // triangles are generated------------------------------
        connList = newPolys;
        clippedList = clippedPolys;
        break;

    } // switch

    cell->Clip(this->Value, cellScalars, this->Locator, connList, inPD, outPD, inCD, cellId, outCD,
      this->InsideOut);

    if (this->GenerateClippedOutput)
    {
      cell->Clip(this->Value, cellScalars, this->Locator, clippedList, inPD, outPD, inCD, cellId,
        outClippedCD, !this->InsideOut);
    }

    if (!(cellId % updateTime))
    {
      this->UpdateProgress(static_cast<double>(cellId) / numCells);
      abort = this->CheckAbort();
    }
  } // for each cell
  cell->Delete();

  vtkDebugMacro(<< "Created: " << newPoints->GetNumberOfPoints() << " points, "
                << newVerts->GetNumberOfCells() << " verts, " << newLines->GetNumberOfCells()
                << " lines, " << newPolys->GetNumberOfCells() << " polys");

  if (this->GenerateClippedOutput)
  {
    vtkDebugMacro(<< "Created (clipped output): " << clippedVerts->GetNumberOfCells() << " verts, "
                  << clippedLines->GetNumberOfCells() << " lines, "
                  << clippedPolys->GetNumberOfCells() << " triangles");
  }

  // Update ourselves.  Because we don't know upfront how many verts, lines,
  // polys we've created, take care to reclaim memory.
  //
  if (this->ClipFunction)
  {
    clipScalars->Delete();
    inPD->Delete();
  }

  if (newVerts->GetNumberOfCells())
  {
    output->SetVerts(newVerts);
  }
  newVerts->Delete();

  if (newLines->GetNumberOfCells())
  {
    output->SetLines(newLines);
  }
  newLines->Delete();

  if (newPolys->GetNumberOfCells())
  {
    output->SetPolys(newPolys);
  }
  newPolys->Delete();

  if (this->GenerateClippedOutput)
  {
    this->GetClippedOutput()->SetPoints(newPoints);

    if (clippedVerts->GetNumberOfCells())
    {
      this->GetClippedOutput()->SetVerts(clippedVerts);
    }
    clippedVerts->Delete();

    if (clippedLines->GetNumberOfCells())
    {
      this->GetClippedOutput()->SetLines(clippedLines);
    }
    clippedLines->Delete();

    if (clippedPolys->GetNumberOfCells())
    {
      this->GetClippedOutput()->SetPolys(clippedPolys);
    }
    clippedPolys->Delete();

    this->GetClippedOutput()->GetPointData()->PassData(outPD);
    this->GetClippedOutput()->Squeeze();
  }

  output->SetPoints(newPoints);
  newPoints->Delete();
  cellScalars->Delete();

  this->Locator->Initialize(); // release any extra memory
  output->Squeeze();

  return 1;
}

//------------------------------------------------------------------------------
// Specify a spatial locator for merging points. By default,
// an instance of vtkMergePoints is used.
void vtkClipPolyData::SetLocator(vtkIncrementalPointLocator* locator)
{
  if (this->Locator == locator)
  {
    return;
  }

  if (this->Locator)
  {
    this->Locator->UnRegister(this);
    this->Locator = nullptr;
  }

  if (locator)
  {
    locator->Register(this);
  }

  this->Locator = locator;
  this->Modified();
}

//------------------------------------------------------------------------------
void vtkClipPolyData::CreateDefaultLocator()
{
  if (this->Locator == nullptr)
  {
    this->Locator = vtkMergePoints::New();
  }
}

//------------------------------------------------------------------------------
void vtkClipPolyData::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);

  if (this->ClipFunction)
  {
    os << indent << "Clip Function: " << this->ClipFunction << "\n";
  }
  else
  {
    os << indent << "Clip Function: (none)\n";
  }
  os << indent << "InsideOut: " << (this->InsideOut ? "On\n" : "Off\n");
  os << indent << "Value: " << this->Value << "\n";
  if (this->Locator)
  {
    os << indent << "Locator: " << this->Locator << "\n";
  }
  else
  {
    os << indent << "Locator: (none)\n";
  }

  os << indent << "Generate Clip Scalars: " << (this->GenerateClipScalars ? "On\n" : "Off\n");

  os << indent << "Generate Clipped Output: " << (this->GenerateClippedOutput ? "On\n" : "Off\n");

  os << indent << "Output Points Precision: " << this->OutputPointsPrecision << "\n";
}
VTK_ABI_NAMESPACE_END
