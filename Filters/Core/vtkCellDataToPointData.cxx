// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkCellDataToPointData.h"

#include "vtkAbstractCellLinks.h"
#include "vtkArrayDispatch.h"
#include "vtkArrayListTemplate.h" // For processing attribute data
#include "vtkCartesianGrid.h"
#include "vtkCell.h"
#include "vtkCellData.h"
#include "vtkCellTypeUtilities.h"
#include "vtkDataArrayRange.h"
#include "vtkDataSet.h"
#include "vtkCVISTASMPDefaults.h" // cvista: opt into default multithreading (bit-exact)
#include "vtkIdList.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkNew.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkSMPThreadLocalObject.h"
#include "vtkSMPTools.h"
#include "vtkSmartPointer.h"
#include "vtkStaticCellLinks.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkStructuredData.h"
#include "vtkStructuredGrid.h"
#include "vtkUniformGrid.h"
#include "vtkUnsignedIntArray.h"
#include "vtkUnstructuredGrid.h"

#include <algorithm>
#include <functional>
#include <set>

#define VTK_MAX_CELLS_PER_POINT 4096

VTK_ABI_NAMESPACE_BEGIN
vtkObjectFactoryNewMacro(vtkCellDataToPointData);

namespace
{

//------------------------------------------------------------------------------
// Optimized code for vtkUnstructuredGrid/vtkPolyData. It's waaaay faster than the more
// general path.
template <typename TCellLinks>
struct UnstructuredDataCD2PD
{
  TCellLinks* Links;
  ArrayList Arrays;

  UnstructuredDataCD2PD(vtkIdType numPts, vtkCellData* inDA, vtkPointData* outDA, TCellLinks* links)
    : Links(links)
  {
    this->Arrays.AddArrays(numPts, inDA, outDA);
  }

  void operator()(vtkIdType beginPointId, vtkIdType endPointId)
  {
    vtkIdType ncells;
    for (vtkIdType pointId = beginPointId; pointId < endPointId; ++pointId)
    {
      if ((ncells = this->Links->GetNcells(pointId)) > 0)
      {
        auto cells = this->Links->GetCells(pointId);
        this->Arrays.Average(ncells, cells, pointId);
      }
      else
      {
        this->Arrays.AssignNullValue(pointId);
      }
    }
  }
};

//------------------------------------------------------------------------------
// Take care of dispatching to the functor using an abstract cell links.
void FastUnstructuredDataACL(
  vtkIdType numPts, vtkAbstractCellLinks* links, vtkCellData* cfl, vtkPointData* pd)
{
  assert(links != nullptr);
  if (auto staticCellLinks = vtkStaticCellLinks::SafeDownCast(links))
  {
    UnstructuredDataCD2PD<vtkStaticCellLinks> cd2pd(numPts, cfl, pd, staticCellLinks);
    cvista::RunSafeFilterParallel([&]() { vtkSMPTools::For(0, numPts, cd2pd); });
  }
  else // vtkCellLinks
  {
    auto cellLinks = vtkCellLinks::SafeDownCast(links);
    UnstructuredDataCD2PD<vtkCellLinks> cd2pd(numPts, cfl, pd, cellLinks);
    cvista::RunSafeFilterParallel([&]() { vtkSMPTools::For(0, numPts, cd2pd); });
  }
}

//------------------------------------------------------------------------------
// Take care of dispatching to the functor using a static cell links template instance.
template <typename TInput>
void FastUnstructuredDataSCLT(
  vtkIdType connectivitySize, TInput* input, vtkCellData* cfl, vtkPointData* pd)
{
  const auto numberOfPoints = input->GetNumberOfPoints();
  const auto numberOfCells = input->GetNumberOfCells();
  auto linksType =
    vtkAbstractCellLinks::ComputeType(numberOfPoints - 1, numberOfCells - 1, connectivitySize);
  // build the appropriate static cell links template instance
  if (linksType == vtkAbstractCellLinks::STATIC_CELL_LINKS_USHORT)
  {
    using TCellLinks = vtkStaticCellLinksTemplate<unsigned short>;
    TCellLinks cellLinks;
    cellLinks.BuildLinks(input);
    UnstructuredDataCD2PD<TCellLinks> cd2pd(numberOfPoints, cfl, pd, &cellLinks);
    cvista::RunSafeFilterParallel([&]() { vtkSMPTools::For(0, numberOfPoints, cd2pd); });
  }
#ifdef VTK_USE_64BIT_IDS
  else if (linksType == vtkAbstractCellLinks::STATIC_CELL_LINKS_UINT)
  {
    using TCellLinks = vtkStaticCellLinksTemplate<unsigned int>;
    TCellLinks cellLinks;
    cellLinks.BuildLinks(input);
    UnstructuredDataCD2PD<TCellLinks> cd2pd(numberOfPoints, cfl, pd, &cellLinks);
    cvista::RunSafeFilterParallel([&]() { vtkSMPTools::For(0, numberOfPoints, cd2pd); });
  }
#endif
  else
  {
    using TCellLinks = vtkStaticCellLinksTemplate<vtkIdType>;
    TCellLinks cellLinks;
    cellLinks.BuildLinks(input);
    UnstructuredDataCD2PD<TCellLinks> cd2pd(numberOfPoints, cfl, pd, &cellLinks);
    cvista::RunSafeFilterParallel([&]() { vtkSMPTools::For(0, numberOfPoints, cd2pd); });
  }
}

//------------------------------------------------------------------------------
// Helper template function that implements the major part of the algorithm
// which will be expanded by the vtkTemplateMacro. The template function is
// provided so that coverage test can cover this function. This approach is
// slow: it's non-threaded; uses a slower vtkDataSet API; and most
// unfortunately, accommodates the ContributingCellOption which is not a
// common workflow.
struct Spread
{
  template <typename SrcArrayT, typename DstArrayT>
  void operator()(SrcArrayT* const srcarray, DstArrayT* const dstarray, vtkDataSet* const src,
    vtkUnsignedIntArray* const num, vtkIdType ncells, vtkIdType npoints, vtkIdType ncomps,
    int highestCellDimension, int contributingCellOption, vtkCellDataToPointData* filter) const
  {
    // Both arrays will have the same value type:
    using T = vtk::GetAPIType<SrcArrayT>;

    // zero initialization
    std::fill_n(vtk::DataArrayValueRange(dstarray).begin(), npoints * ncomps, T(0));

    const auto srcTuples = vtk::DataArrayTupleRange(srcarray);
    auto dstTuples = vtk::DataArrayTupleRange(dstarray);
    vtkIdType checkAbortInterval;

    // accumulate
    if (contributingCellOption != vtkCellDataToPointData::Patch)
    {
      vtkNew<vtkIdList> pointIds;
      checkAbortInterval = std::min(ncells / 10 + 1, (vtkIdType)1000);
      for (vtkIdType cid = 0; cid < ncells; ++cid)
      {
        if (cid % checkAbortInterval == 0 && filter->CheckAbort())
        {
          break;
        }
        int dimension = vtkCellTypeUtilities::GetDimension(src->GetCellType(cid));
        if (dimension >= highestCellDimension)
        {
          const auto srcTuple = srcTuples[cid];
          src->GetCellPoints(cid, pointIds);
          for (vtkIdType i = 0, I = pointIds->GetNumberOfIds(); i < I; ++i)
          {
            const vtkIdType ptId = pointIds->GetId(i);
            auto dstTuple = dstTuples[ptId];
            // accumulate cell data to point data <==> point_data += cell_data
            std::transform(srcTuple.cbegin(), srcTuple.cend(), dstTuple.cbegin(), dstTuple.begin(),
              std::plus<T>());
          }
        }
      }
      // average

      checkAbortInterval = std::min(npoints / 10 + 1, (vtkIdType)1000);
      for (vtkIdType pid = 0; pid < npoints; ++pid)
      {
        if (pid % checkAbortInterval == 0 && filter->CheckAbort())
        {
          break;
        }
        // guard against divide by zero
        if (unsigned int const denom = num->GetValue(pid))
        {
          // divide point data by the number of cells using it <==>
          // point_data /= denum
          auto dstTuple = dstTuples[pid];
          std::transform(dstTuple.cbegin(), dstTuple.cend(), dstTuple.begin(),
            [denom](T value) { return value / denom; });
        }
      }
    }
    else
    { // compute over cell patches
      vtkNew<vtkIdList> cellsOnPoint;
      std::vector<T> data(4 * ncomps);
      checkAbortInterval = std::min(npoints / 10 + 1, (vtkIdType)1000);
      for (vtkIdType pid = 0; pid < npoints; ++pid)
      {
        if (pid % checkAbortInterval == 0 && filter->CheckAbort())
        {
          break;
        }
        std::fill(data.begin(), data.end(), 0);
        T numPointCells[4] = { 0, 0, 0, 0 };
        // Get all cells touching this point.
        src->GetPointCells(pid, cellsOnPoint);
        vtkIdType numPatchCells = cellsOnPoint->GetNumberOfIds();
        for (vtkIdType pc = 0; pc < numPatchCells; pc++)
        {
          vtkIdType cellId = cellsOnPoint->GetId(pc);
          int cellDimension = src->GetCell(cellId)->GetCellDimension();
          numPointCells[cellDimension] += 1;
          const auto srcTuple = srcTuples[cellId];
          for (int comp = 0; comp < ncomps; comp++)
          {
            data[comp + ncomps * cellDimension] += srcTuple[comp];
          }
        }
        auto dstTuple = dstTuples[pid];
        for (int dimension = 3; dimension >= 0; dimension--)
        {
          if (numPointCells[dimension])
          {
            for (int comp = 0; comp < ncomps; comp++)
            {
              dstTuple[comp] = data[comp + dimension * ncomps] / numPointCells[dimension];
            }
            break;
          }
        }
      }
    }
  }
};

//------------------------------------------------------------------------------
// Bit-exact, devirtualized replacement for the per-point call
//   input->GetPointCells(ptId, cellIds)
// when the input is a structured dataset whose GetPointCells routes to
// vtkStructuredData::GetPointCells (vtkImageData/vtkUniformGrid/
// vtkRectilinearGrid via vtkCartesianGrid, and vtkStructuredGrid).
//
// This mirrors vtkStructuredData::GetPointCells EXACTLY: the same offset
// table, the same iteration order, and the same cellId formula. It therefore
// produces an identical cellIds list (identical values in identical order) for
// every point. The only differences are (a) the structured dimensions are
// fetched once by the caller instead of per call, (b) the call is direct
// rather than through the vtkDataSet virtual table, and (c) ids are written
// into a pre-sized buffer with the bookkeeping hoisted out of the inner loop,
// avoiding vtkIdList::Reset()/InsertNextId() capacity churn. The resulting
// cellIds are consumed by the unchanged outPD->InterpolatePoint(), so the
// floating-point averaging order is byte-for-byte unchanged.
inline void StructuredGetPointCells(vtkIdType ptId, vtkIdList* cellIds, const int dim[3])
{
  // Match vtkStructuredData::GetPointCells offset table and order exactly.
  static const int offset[8][3] = { { -1, 0, 0 }, { -1, -1, 0 }, { -1, -1, -1 }, { -1, 0, -1 },
    { 0, 0, 0 }, { 0, -1, 0 }, { 0, -1, -1 }, { 0, 0, -1 } };

  vtkIdType cellDim[3];
  for (int i = 0; i < 3; i++)
  {
    cellDim[i] = dim[i] - 1;
    if (cellDim[i] == 0)
    {
      cellDim[i] = 1;
    }
  }

  const int ptLoc[3] = { static_cast<int>(ptId % dim[0]),
    static_cast<int>((ptId / dim[0]) % dim[1]),
    static_cast<int>(ptId / (static_cast<vtkIdType>(dim[0]) * dim[1])) };

  // Pre-size to the maximum (8) and write directly; trim at the end. This
  // avoids the per-id Reset()/InsertNextId() capacity check while preserving
  // the exact same sequence of appended ids.
  cellIds->SetNumberOfIds(8);
  vtkIdType* ids = cellIds->GetPointer(0);
  vtkIdType count = 0;
  for (int j = 0; j < 8; j++)
  {
    int cellLoc[3];
    int i;
    for (i = 0; i < 3; i++)
    {
      cellLoc[i] = ptLoc[i] + offset[j][i];
      if (cellLoc[i] < 0 || cellLoc[i] >= cellDim[i])
      {
        break;
      }
    }
    if (i >= 3) // add cell
    {
      ids[count++] = cellLoc[0] + cellLoc[1] * cellDim[0] + cellLoc[2] * cellDim[0] * cellDim[1];
    }
  }
  cellIds->SetNumberOfIds(count);
}

} // end anonymous namespace

//----------------------------------------------------------------------------
// Implementation support
class vtkCellDataToPointData::Internals
{
public:
  std::set<std::string> CellDataArrays;

  // Special traversal algorithm for vtkUniformGrid and vtkRectilinearGrid to support blanking
  // points will not have more than 8 cells for either of these data sets
  template <typename T>
  int InterpolatePointDataWithMask(vtkCellDataToPointData* filter, T* input, vtkDataSet* output)
  {
    vtkNew<vtkIdList> allCellIds;
    allCellIds->Allocate(8);
    vtkNew<vtkIdList> cellIds;
    cellIds->Allocate(8);

    const vtkIdType numberOfPoints = input->GetNumberOfPoints();

    vtkCellData* inCD = input->GetCellData();
    vtkPointData* outPD = output->GetPointData();

    // Copy all existing cell fields into a temporary cell data array,
    // unless the SelectCellDataArrays option is active.
    vtkNew<vtkCellData> processedCellData;
    if (!filter->GetProcessAllArrays())
    {
      for (const auto& name : this->CellDataArrays)
      {
        vtkAbstractArray* array = inCD->GetAbstractArray(name.c_str());
        if (!array)
        {
          vtkWarningWithObjectMacro(filter, "cell data array name not found.");
          continue;
        }
        processedCellData->AddArray(array);
      }
    }
    else
    {
      processedCellData->ShallowCopy(inCD);
    }

    outPD->InterpolateAllocate(processedCellData, numberOfPoints);

    double weights[8];

    bool abort = false;
    vtkIdType progressInterval = numberOfPoints / 20 + 1;
    for (vtkIdType ptId = 0; ptId < numberOfPoints && !abort; ptId++)
    {
      if (!(ptId % progressInterval))
      {
        filter->UpdateProgress(static_cast<double>(ptId) / numberOfPoints);
        abort = filter->CheckAbort();
      }
      input->GetPointCells(ptId, allCellIds);
      cellIds->Reset();
      // Only consider cells that are not masked:
      for (vtkIdType cId = 0; cId < allCellIds->GetNumberOfIds(); ++cId)
      {
        vtkIdType curCell = allCellIds->GetId(cId);
        if (input->IsCellVisible(curCell))
        {
          cellIds->InsertNextId(curCell);
        }
      }

      vtkIdType numCells = cellIds->GetNumberOfIds();

      if (numCells > 0)
      {
        double weight = 1.0 / numCells;
        for (vtkIdType cellId = 0; cellId < numCells; cellId++)
        {
          weights[cellId] = weight;
        }
        outPD->InterpolatePoint(processedCellData, ptId, cellIds, weights);
      }
      else
      {
        outPD->NullData(ptId);
      }
    }

    return 1;
  }
};

//------------------------------------------------------------------------------
// Instantiate object so that cell data is not passed to output.
vtkCellDataToPointData::vtkCellDataToPointData()
{
  this->PassCellData = false;
  this->ContributingCellOption = vtkCellDataToPointData::All;
  this->ProcessAllArrays = true;
  this->PieceInvariant = true;
  this->Implementation = new Internals();
}

//------------------------------------------------------------------------------
vtkCellDataToPointData::~vtkCellDataToPointData()
{
  delete this->Implementation;
}

//------------------------------------------------------------------------------
void vtkCellDataToPointData::AddCellDataArray(const char* name)
{
  if (!name)
  {
    vtkErrorMacro("name cannot be null.");
    return;
  }

  this->Implementation->CellDataArrays.insert(std::string(name));
  this->Modified();
}

//------------------------------------------------------------------------------
void vtkCellDataToPointData::RemoveCellDataArray(const char* name)
{
  if (!name)
  {
    vtkErrorMacro("name cannot be null.");
    return;
  }

  this->Implementation->CellDataArrays.erase(name);
  this->Modified();
}

//------------------------------------------------------------------------------
void vtkCellDataToPointData::ClearCellDataArrays()
{
  if (!this->Implementation->CellDataArrays.empty())
  {
    this->Modified();
  }
  this->Implementation->CellDataArrays.clear();
}

//------------------------------------------------------------------------------
vtkIdType vtkCellDataToPointData::GetNumberOfCellArraysToProcess()
{
  return static_cast<vtkIdType>(this->Implementation->CellDataArrays.size());
}

//------------------------------------------------------------------------------
void vtkCellDataToPointData::GetCellArraysToProcess(const char* names[])
{
  for (const auto& n : this->Implementation->CellDataArrays)
  {
    *names = n.c_str();
    ++names;
  }
}

//------------------------------------------------------------------------------
int vtkCellDataToPointData::RequestData(
  vtkInformation*, vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  vtkDataSet* input = vtkDataSet::GetData(inputVector[0]);
  vtkDataSet* output = vtkDataSet::GetData(outputVector);

  vtkDebugMacro(<< "Mapping cell data to point data");

  // Special traversal algorithm for unstructured data such as vtkPolyData
  // and vtkUnstructuredGrid.
  if (input->IsA("vtkUnstructuredGrid") || input->IsA("vtkPolyData"))
  {
    return this->RequestDataForUnstructuredData(nullptr, inputVector, outputVector);
  }

  // First, copy the input to the output as a starting point
  output->CopyStructure(input);

  vtkPointData* inPD = input->GetPointData();
  vtkCellData* inCD = input->GetCellData();
  vtkPointData* outPD = output->GetPointData();
  vtkCellData* outCD = output->GetCellData();

  // Pass the point data first. The fields and attributes
  // which also exist in the cell data of the input will
  // be over-written during CopyAllocate
  outPD->PassData(inPD);
  outPD->CopyFieldOff(vtkDataSetAttributes::GhostArrayName());

  if (input->GetNumberOfPoints() < 1)
  {
    vtkDebugMacro(<< "No input point data!");
    return 1;
  }

  // Do the interpolation, taking care of masked cells if needed.
  vtkStructuredGrid* sGrid = vtkStructuredGrid::SafeDownCast(input);
  vtkUniformGrid* uniformGrid = vtkUniformGrid::SafeDownCast(input);
  int result;
  if (sGrid && sGrid->HasAnyBlankCells())
  {
    result = this->Implementation->InterpolatePointDataWithMask(this, sGrid, output);
  }
  else if (uniformGrid && uniformGrid->HasAnyBlankCells())
  {
    result = this->Implementation->InterpolatePointDataWithMask(this, uniformGrid, output);
  }
  else
  {
    result = this->InterpolatePointData(input, output);
  }

  if (result == 0)
  {
    return 0;
  }

  if (!this->PassCellData)
  {
    outCD->CopyAllOff();
    outCD->CopyFieldOn(vtkDataSetAttributes::GhostArrayName());
  }
  outCD->PassData(inCD);

  return 1;
}

//------------------------------------------------------------------------------
int vtkCellDataToPointData::RequestUpdateExtent(
  vtkInformation*, vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  if (!this->PieceInvariant)
  {
    // I believe the default input update extent
    // is set to the input update extent.
    return 1;
  }

  // Technically, this code is only correct for pieces extent types.  However,
  // since this class is pretty inefficient for data types that use 3D extents,
  // we'll punt on the ghost levels for them, too.

  // get the info objects
  vtkInformation* inInfo = inputVector[0]->GetInformationObject(0);
  vtkInformation* outInfo = outputVector->GetInformationObject(0);

  int piece = outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_PIECE_NUMBER());
  int numPieces = outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_NUMBER_OF_PIECES());
  int ghostLevels = outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_NUMBER_OF_GHOST_LEVELS());

  if (numPieces > 1)
  {
    ++ghostLevels;
  }

  inInfo->Set(vtkStreamingDemandDrivenPipeline::UPDATE_PIECE_NUMBER(), piece);
  inInfo->Set(vtkStreamingDemandDrivenPipeline::UPDATE_NUMBER_OF_PIECES(), numPieces);
  inInfo->Set(vtkStreamingDemandDrivenPipeline::UPDATE_NUMBER_OF_GHOST_LEVELS(), ghostLevels);
  inInfo->Set(vtkStreamingDemandDrivenPipeline::EXACT_EXTENT(), 1);

  return 1;
}

//------------------------------------------------------------------------------
void vtkCellDataToPointData::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);

  os << indent << "PassCellData: " << (this->PassCellData ? "On\n" : "Off\n");
  os << indent << "ContributingCellOption: " << this->ContributingCellOption << endl;
  os << indent << "PieceInvariant: " << (this->PieceInvariant ? "On\n" : "Off\n");
}

//----------------------------------------------------------------------------
// In general the method below is quite slow due to ContributingCellOption
// considerations. If the ContributingCellOption is "All", and the dataset
// type is unstructured, then a threaded, tuned approach is used.
int vtkCellDataToPointData::RequestDataForUnstructuredData(
  vtkInformation*, vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  vtkPointSet* input = vtkPointSet::GetData(inputVector[0]);
  vtkPointSet* output = vtkPointSet::GetData(outputVector);

  const vtkIdType numberOfCells = input->GetNumberOfCells();
  const vtkIdType numberOfPoints = input->GetNumberOfPoints();
  if (numberOfCells < 1 || numberOfPoints < 1)
  {
    vtkDebugMacro(<< "No input data!");
    return 1;
  }

  // Begin by performing the tasks common to both the slow and fast paths.

  // First, copy the input structure (geometry and topology) to the output as
  // a starting point.
  output->CopyStructure(input);

  vtkCellData* inCD = input->GetCellData();
  vtkPointData* outPD = output->GetPointData();
  vtkCellData* outCD = output->GetCellData();

  // Pass the point data first. The fields and attributes which also exist in
  // the cell data of the input will be over-written during CopyAllocate
  outPD->CopyGlobalIdsOff();
  outPD->PassData(input->GetPointData());
  outPD->CopyFieldOff(vtkDataSetAttributes::GhostArrayName());

  // Copy all existing cell fields into a temporary cell data array,
  // unless the SelectCellDataArrays option is active.
  vtkNew<vtkCellData> processedCellData;
  if (!this->ProcessAllArrays)
  {
    for (const auto& name : this->Implementation->CellDataArrays)
    {
      vtkAbstractArray* array = inCD->GetAbstractArray(name.c_str());
      if (!array)
      {
        vtkWarningMacro("cell data array name not found.");
        continue;
      }
      processedCellData->AddArray(array);
    }
  }
  else
  {
    processedCellData->ShallowCopy(inCD);
  }

  // Remove all fields that are not a data array.
  for (vtkIdType fid = processedCellData->GetNumberOfArrays(); fid--;)
  {
    if (!vtkDataArray::FastDownCast(processedCellData->GetAbstractArray(fid)))
    {
      processedCellData->RemoveArray(fid);
    }
  }

  outPD->InterpolateAllocate(processedCellData, numberOfPoints);

  // Pass the input cell data to the output as appropriate.
  if (!this->PassCellData)
  {
    outCD->CopyAllOff();
    outCD->CopyFieldOn(vtkDataSetAttributes::GhostArrayName());
  }
  outCD->PassData(inCD);

  // Now perform the averaging operation.

  // Use a much faster approach for the "All" ContributingCellOption, and
  // unstructured datasets. A common workflow requiring maximum performance.
  if (this->ContributingCellOption == vtkCellDataToPointData::All)
  {
    if (auto uGrid = vtkUnstructuredGrid::SafeDownCast(input))
    {
      if (uGrid->GetLinks()) // if links are present use them
      {
        uGrid->BuildLinks(); // ensure links are up to date
        FastUnstructuredDataACL(numberOfPoints, uGrid->GetLinks(), processedCellData, outPD);
      }
      else // otherwise create links with the minimum size
      {
        vtkIdType connectivitySize = uGrid->GetCells()->GetNumberOfConnectivityIds();
        FastUnstructuredDataSCLT(connectivitySize, uGrid, processedCellData, outPD);
      }
      return 1;
    }
    else // polydata
    {
      auto polyData = vtkPolyData::SafeDownCast(input);
      if (polyData->GetLinks()) // if links are present use them
      {
        polyData->BuildLinks(); // ensure links are up to date
        FastUnstructuredDataACL(numberOfPoints, polyData->GetLinks(), processedCellData, outPD);
      }
      else // otherwise create links with the minimum size
      {
        auto verts = polyData->GetVerts();
        auto lines = polyData->GetLines();
        auto polys = polyData->GetPolys();
        auto strips = polyData->GetStrips();
        vtkIdType connectivitySize = 0;
        connectivitySize += verts ? verts->GetNumberOfConnectivityIds() : 0;
        connectivitySize += lines ? lines->GetNumberOfConnectivityIds() : 0;
        connectivitySize += polys ? polys->GetNumberOfConnectivityIds() : 0;
        connectivitySize += strips ? strips->GetNumberOfConnectivityIds() : 0;
        FastUnstructuredDataSCLT(connectivitySize, polyData, processedCellData, outPD);
      }
      return 1;
    }
  } // fast path

  // If necessary, begin the slow, more general path.

  // To a large extent the loops immediately following are a serial version
  // of BuildLinks() found in vtkUnstructuredGrid and vtkPolyData. The code
  // below could be threaded if necessary. Count the number of cells
  // associated with each point. If we are doing patches though we will do
  // that later on.
  vtkSmartPointer<vtkUnsignedIntArray> num;
  int highestCellDimension = 0;
  if (this->ContributingCellOption != vtkCellDataToPointData::Patch)
  {
    num = vtkSmartPointer<vtkUnsignedIntArray>::New();
    num->SetNumberOfValues(numberOfPoints);
    num->FillValue(0);
    if (this->ContributingCellOption == vtkCellDataToPointData::DataSetMax)
    {
      int maxDimension = input->IsA("vtkPolyData") == 1 ? 2 : 3;
      for (vtkIdType i = 0; i < numberOfCells; i++)
      {
        int dim = vtkCellTypeUtilities::GetDimension(input->GetCellType(i));
        if (dim > highestCellDimension)
        {
          highestCellDimension = dim;
          if (highestCellDimension == maxDimension)
          {
            break;
          }
        }
      }
    }
    vtkNew<vtkIdList> pids;
    for (vtkIdType cid = 0; cid < numberOfCells; ++cid)
    {
      if (input->GetCell(cid)->GetCellDimension() >= highestCellDimension)
      {
        input->GetCellPoints(cid, pids);
        for (vtkIdType i = 0, I = pids->GetNumberOfIds(); i < I; ++i)
        {
          vtkIdType const pid = pids->GetId(i);
          num->SetValue(pid, num->GetValue(pid) + 1);
        }
      }
    }
  }

  const auto nfields = processedCellData->GetNumberOfArrays();
  int fid = 0;
  auto f = [this, &fid, nfields, numberOfPoints, input, num, numberOfCells, highestCellDimension](
             vtkAbstractArray* aa_srcarray, vtkAbstractArray* aa_dstarray)
  {
    // update progress and check for an abort request.
    this->UpdateProgress((fid + 1.0) / nfields);
    ++fid;

    vtkDataArray* const srcarray = vtkDataArray::FastDownCast(aa_srcarray);
    vtkDataArray* const dstarray = vtkDataArray::FastDownCast(aa_dstarray);
    if (srcarray && dstarray)
    {
      dstarray->SetNumberOfTuples(numberOfPoints);
      vtkIdType const ncomps = srcarray->GetNumberOfComponents();

      Spread worker;
      using Dispatcher = vtkArrayDispatch::Dispatch2SameValueType;
      if (!Dispatcher::Execute(srcarray, dstarray, worker, input, num, numberOfCells,
            numberOfPoints, ncomps, highestCellDimension, this->ContributingCellOption, this))
      { // fallback for unknown arrays:
        worker(srcarray, dstarray, input, num, numberOfCells, numberOfPoints, ncomps,
          highestCellDimension, this->ContributingCellOption, this);
      }
    }
  };

  // Cell field list constructed from the filtered cell data array
  vtkDataSetAttributes::FieldList cfl(1);
  cfl.InitializeFieldList(processedCellData);
  if (processedCellData != nullptr && outPD != nullptr)
  {
    cfl.TransformData(0, processedCellData, outPD, f);
  }

  return 1; // slow path
}

//------------------------------------------------------------------------------
int vtkCellDataToPointData::InterpolatePointData(vtkDataSet* input, vtkDataSet* output)
{
  const vtkIdType numberOfPoints = input->GetNumberOfPoints();

  vtkCellData* inCD = input->GetCellData();
  vtkPointData* outPD = output->GetPointData();

  // Copy all existing cell fields into a temporary cell data array,
  // unless the SelectCellDataArrays option is active.
  vtkNew<vtkCellData> processedCellData;
  if (!this->ProcessAllArrays)
  {
    for (const auto& name : this->Implementation->CellDataArrays)
    {
      vtkAbstractArray* array = inCD->GetAbstractArray(name.c_str());
      if (!array)
      {
        vtkWarningMacro("cell data array name not found.");
        continue;
      }
      processedCellData->AddArray(array);
    }
  }
  else
  {
    processedCellData->ShallowCopy(inCD);
  }

  outPD->InterpolateAllocate(processedCellData, numberOfPoints);

  // cvista: pre-size EVERY output point-data array to numberOfPoints tuples so the
  // threaded loop below is a pure index-addressed store. InterpolateAllocate()
  // only reserves capacity (MaxId == -1); without this presize the first
  // InterpolateTuple(ptId,...)/InsertTuple(ptId,...) on each array would bump
  // MaxId / realloc, which is not thread-safe. NullData() likewise InsertTuple()s
  // into every array in outPD (not just the interpolated ones), so all arrays --
  // including the ones already passed through from the input point data -- must
  // be sized to numberOfPoints. The pass-through arrays already hold exactly
  // numberOfPoints tuples (one per input point), so resizing them is a no-op.
  // After this, every per-point write targets an existing tuple: no realloc, no
  // MaxId bump, no shared mutable state across iterations.
  for (int i = 0; i < outPD->GetNumberOfArrays(); ++i)
  {
    if (vtkAbstractArray* outArray = outPD->GetAbstractArray(i))
    {
      outArray->SetNumberOfTuples(numberOfPoints);
    }
  }

  // Fast, bit-exact incident-cell traversal for structured inputs. Both
  // vtkCartesianGrid (vtkImageData/vtkUniformGrid/vtkRectilinearGrid) and
  // vtkStructuredGrid route their GetPointCells() through the same
  // vtkStructuredData::GetPointCells(ptId, cellIds, dims). When we recognize
  // such an input we fetch the structured dimensions once here and replicate
  // that traversal directly (see StructuredGetPointCells), bypassing the
  // per-point virtual dispatch and vtkIdList capacity churn. The produced
  // cellIds are identical (same ids, same order), so the downstream averaging
  // performed by InterpolatePoint is byte-for-byte unchanged.
  bool useStructuredFastPath = false;
  int structuredDims[3] = { 0, 0, 0 };
  if (auto* cartesianGrid = vtkCartesianGrid::SafeDownCast(input))
  {
    cartesianGrid->GetDimensions(structuredDims);
    useStructuredFastPath = true;
  }
  else if (auto* structuredGrid = vtkStructuredGrid::SafeDownCast(input))
  {
    structuredGrid->GetDimensions(structuredDims);
    useStructuredFastPath = true;
  }

  // cvista: thread the per-output-point interpolation loop (byte-exact under any
  // thread count). The output is index-addressed by ptId, so disjoint sub-ranges
  // assigned to different threads write to disjoint, pre-sized tuples -- zero
  // write conflict and the emission order is preserved exactly. The per-point
  // average sums the same (<= 8) terms in the same index order regardless of how
  // the range is partitioned, so there is no floating-point reassociation across
  // iterations. cellIds and the weights buffer are thread-local scratch.
  struct Interpolator
  {
    vtkCellDataToPointData* Filter;
    vtkDataSet* Input;
    vtkPointData* OutPD;
    vtkCellData* ProcessedCellData;
    vtkIdType NumberOfPoints;
    bool UseStructuredFastPath;
    const int* StructuredDims;
    vtkSMPThreadLocalObject<vtkIdList> TLCellIds;

    void Initialize() { this->TLCellIds.Local()->Allocate(VTK_MAX_CELLS_PER_POINT); }

    void operator()(vtkIdType beginPtId, vtkIdType endPtId)
    {
      vtkIdList* cellIds = this->TLCellIds.Local();
      double weights[VTK_MAX_CELLS_PER_POINT];
      const bool isFirst = vtkSMPTools::GetSingleThread();
      const vtkIdType checkInterval =
        std::min((endPtId - beginPtId) / 20 + 1, static_cast<vtkIdType>(1000));
      for (vtkIdType ptId = beginPtId; ptId < endPtId; ++ptId)
      {
        if (ptId % checkInterval == 0)
        {
          if (isFirst)
          {
            this->Filter->UpdateProgress(static_cast<double>(ptId) / this->NumberOfPoints);
            this->Filter->CheckAbort();
          }
          if (this->Filter->GetAbortOutput())
          {
            break;
          }
        }

        if (this->UseStructuredFastPath)
        {
          StructuredGetPointCells(ptId, cellIds, this->StructuredDims);
        }
        else
        {
          this->Input->GetPointCells(ptId, cellIds);
        }
        vtkIdType numCells = cellIds->GetNumberOfIds();

        if (numCells > 0 && numCells < VTK_MAX_CELLS_PER_POINT)
        {
          double weight = 1.0 / numCells;
          for (vtkIdType cellId = 0; cellId < numCells; cellId++)
          {
            weights[cellId] = weight;
          }
          this->OutPD->InterpolatePoint(this->ProcessedCellData, ptId, cellIds, weights);
        }
        else
        {
          this->OutPD->NullData(ptId);
        }
      }
    }

    void Reduce() {}
  };

  // For the (rare) non-structured fallback that still routes here, prime any
  // lazily-built incident-cell structures on the main thread before the parallel
  // region so the first GetPointCells() call cannot race. Structured inputs take
  // the pure StructuredGetPointCells() path and need no priming.
  if (!useStructuredFastPath && numberOfPoints > 0)
  {
    vtkNew<vtkIdList> primeIds;
    input->GetPointCells(0, primeIds);
  }

  Interpolator interp;
  interp.Filter = this;
  interp.Input = input;
  interp.OutPD = outPD;
  interp.ProcessedCellData = processedCellData;
  interp.NumberOfPoints = numberOfPoints;
  interp.UseStructuredFastPath = useStructuredFastPath;
  interp.StructuredDims = structuredDims;

  cvista::RunSafeFilterParallel([&]() { vtkSMPTools::For(0, numberOfPoints, interp); });

  return 1;
}
VTK_ABI_NAMESPACE_END
