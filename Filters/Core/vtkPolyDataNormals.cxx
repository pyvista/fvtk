// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkPolyDataNormals.h"

#include "vtkAlgorithmOutput.h"
#include "vtkAtomicMutex.h"
#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkCommand.h"
#include "vtkEventForwarderCommand.h"
#include "vtkFloatArray.h"
#include "vtkIdList.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMath.h"
#include "vtkNew.h"
#include "vtkFVTKSMPDefaults.h"
#include "vtkObjectFactory.h"
#include "vtkOrientPolyData.h"
#include "vtkPointData.h"
#include "vtkPolyData.h"
#include "vtkPolygon.h"
#include "vtkSMPThreadLocalObject.h"
#include "vtkSMPTools.h"
#include "vtkSplitSharpEdgesPolyData.h"
#include "vtkTriangleFilter.h"

#include "vtkAOSDataArrayTemplate.h"

//-----------------------------------------------------------------------------
VTK_ABI_NAMESPACE_BEGIN
vtkStandardNewMacro(vtkPolyDataNormals);

namespace
{
// fvtk bit-exact devirtualization: a raw-pointer cell-normal kernel that hoists
// the per-triangle vtkArrayDispatch out of the loop. The stock path runs a full
// vtkArrayDispatch type-resolution inside vtkPolygon::ComputeNormal for EVERY
// triangle (callgrind: the dispatch ~= the actual normal math). This resolves
// the concrete AOS point-array pointer ONCE outside the loop and computes the
// normal via the SAME double-promoted Newell ops in the SAME order, then
// vtkMath::Normalize -- so the result is BYTE-FOR-BYTE identical (maxULP=0) to
// the dispatched path for triangle cells. It is engaged only when the runtime
// guard below confirms concrete AOS float/double points + shareable triangle
// connectivity; any other input falls back to the stock dispatched path.
template <typename T>
inline void FastTriCellNormal(const T* X, const vtkIdType* p, double n[3])
{
  const T* a = &X[3 * p[0]];
  const T* b = &X[3 * p[1]];
  const T* c = &X[3 * p[2]];
  // Match the triangle case of vtkPolygon's NormalWorker exactly:
  //   v1 = p1 - p0, v2 = p2 - p0 (subtraction in the array value type T,
  //   promoted to double), n = v1 x v2, then vtkMath::Normalize(n).
  // For numPts==3 the worker's commonPointId loop reduces to p0=pts[0],
  // v1=pts[1]-pts[0], v2=pts[2]-pts[0], n = Cross(v1,v2). The degenerate
  // first-edge case (v1==0) yields n={0,0,0} both here (Cross of a zero
  // vector + Normalize-of-zero) and in the stock worker (it returns {0,0,0}),
  // so the result is identical.
  double v1[3] = { double(b[0] - a[0]), double(b[1] - a[1]), double(b[2] - a[2]) };
  double v2[3] = { double(c[0] - a[0]), double(c[1] - a[1]), double(c[2] - a[2]) };
  vtkMath::Cross(v1, v2, n);
  vtkMath::Normalize(n);
}

// Returns true and fills cellNormals[offsetCells..] if the fast path applied to
// every poly. Returns false (after possibly writing some tuples) if any poly is
// not a triangle or the connectivity is not shareable -> caller redoes the whole
// poly range via the stock dispatched path (SetTuple overwrites).
template <typename T>
bool FastCellNormals(vtkAOSDataArrayTemplate<T>* ptsArr, vtkCellArray* polys,
  vtkIdType numPolys, vtkIdType offsetCells, vtkFloatArray* cellNormals)
{
  if (!polys->IsStorageShareable())
  {
    return false;
  }
  const T* X = ptsArr->GetPointer(0);
  float* outN = cellNormals->GetPointer(3 * offsetCells);
  bool ok = true;
  vtkSMPThreadLocalObject<vtkIdList> tlScratch;
  vtkSMPTools::For(0, numPolys,
    [&](vtkIdType begin, vtkIdType end)
    {
      auto scratch = tlScratch.Local();
      vtkIdType npts;
      const vtkIdType* pts = nullptr;
      double n[3];
      for (vtkIdType polyId = begin; polyId < end; ++polyId)
      {
        // IsStorageShareable() guard above => pts points into shared storage
        // (no per-cell copy); scratch is the required fallback arg, unused here.
        polys->GetCellAtId(polyId, npts, pts, scratch);
        if (npts != 3)
        {
          ok = false; // non-triangle -> abandon fast path; caller redoes range.
          return;
        }
        FastTriCellNormal<T>(X, pts, n);
        float* o = &outN[3 * polyId];
        o[0] = static_cast<float>(n[0]);
        o[1] = static_cast<float>(n[1]);
        o[2] = static_cast<float>(n[2]);
      }
    });
  return ok;
}
} // namespace

//-----------------------------------------------------------------------------
// Construct with feature angle=30, splitting and consistency turned on,
// flipNormals turned off, and non-manifold traversal turned on.
vtkPolyDataNormals::vtkPolyDataNormals()
{
  this->FeatureAngle = 30.0;
  this->Splitting = 1;
  this->Consistency = 1;
  this->FlipNormals = 0;
  this->ComputePointNormals = 1;
  this->ComputeCellNormals = 0;
  this->NonManifoldTraversal = 1;
  this->AutoOrientNormals = 0;
  this->OutputPointsPrecision = vtkAlgorithm::DEFAULT_PRECISION;
}

//-----------------------------------------------------------------------------
void vtkPolyDataNormals::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);

  os << indent << "Feature Angle: " << this->GetFeatureAngle() << "\n";
  os << indent << "Splitting: " << (this->GetSplitting() ? "On\n" : "Off\n");
  os << indent << "Consistency: " << (this->GetConsistency() ? "On\n" : "Off\n");
  os << indent << "Flip Normals: " << (this->GetFlipNormals() ? "On\n" : "Off\n");
  os << indent << "Auto Orient Normals: " << (this->GetAutoOrientNormals() ? "On\n" : "Off\n");
  os << indent << "Compute Point Normals: " << (this->GetComputePointNormals() ? "On\n" : "Off\n");
  os << indent << "Compute Cell Normals: " << (this->GetComputeCellNormals() ? "On\n" : "Off\n");
  os << indent
     << "Non-manifold Traversal: " << (this->GetNonManifoldTraversal() ? "On\n" : "Off\n");
  os << indent << "Precision of the output points: " << this->GetOutputPointsPrecision() << "\n";
}

//-----------------------------------------------------------------------------
vtkSmartPointer<vtkFloatArray> vtkPolyDataNormals::GetCellNormals(vtkPolyData* data)
{
  if (auto existingCellNormals = vtkFloatArray::FastDownCast(data->GetCellData()->GetNormals()))
  {
    return existingCellNormals;
  }
  vtkPoints* points = data->GetPoints();
  vtkCellArray* polys = data->GetPolys();
  const vtkIdType numVertices = data->GetNumberOfVerts();
  const vtkIdType numLines = data->GetNumberOfLines();
  const vtkIdType numPolys = data->GetNumberOfPolys();
  const vtkIdType numStrips = data->GetNumberOfStrips();
  // check if the cells are already built
  if (data->NeedToBuildCells())
  {
    data->BuildCells();
  }

  //  Initial pass to compute polygon normals without effects of neighbors
  auto cellNormals = vtkSmartPointer<vtkFloatArray>::New();
  cellNormals->SetName("Normals");
  cellNormals->SetNumberOfComponents(3);
  cellNormals->SetNumberOfTuples(numVertices + numLines + numPolys + numStrips);

  // fvtk: every For below writes cellNormals[cellId] = f(cellId) into pre-sized
  // slots (no append / no reduction), so it is bit-exact under any thread count.
  // Opt the whole cell-normals computation into the fvtk default-on threading
  // (capped at 4, overridable via the VTK SMP APIs). One scope -> the global SMP
  // singleton is mutated once and all three For's inherit it.
  vtkSMPThreadLocalObject<vtkIdList> tlTempCellPointIds;
  fvtk::RunSafeFilterParallel(
    [&]()
    {
      // Set default value for vertices and lines cell normals
      vtkIdType offsetCells = numVertices + numLines;
      vtkSMPTools::For(0, offsetCells,
        [&](vtkIdType begin, vtkIdType end)
        {
          static constexpr float n[3] = { 1.0, 0.0, 0.0 };
          for (vtkIdType cellId = begin; cellId < end; cellId++)
          {
            // add a default value for vertices and lines
            // normals do not have meaningful values, we set them to X
            cellNormals->SetTypedTuple(cellId, n);
          }
        });

      // Compute Cell Normals of polys.
      // Fast path (bit-exact, default-on): when the points are a concrete AOS
      // float/double array, hoist the per-triangle vtkArrayDispatch out of the
      // loop (resolve the typed pointer once) and compute the normal via the
      // same double-promoted Newell ops + vtkMath::Normalize -> byte-identical
      // to the stock dispatched path for triangle meshes. Any non-triangle poly
      // or non-AOS-float/double point array falls back to the stock path below.
      bool computedFast = false;
      if (numPolys > 0 && points)
      {
        if (auto* pf = vtkAOSDataArrayTemplate<float>::FastDownCast(points->GetData()))
        {
          computedFast = FastCellNormals<float>(pf, polys, numPolys, offsetCells, cellNormals);
        }
        else if (auto* pd = vtkAOSDataArrayTemplate<double>::FastDownCast(points->GetData()))
        {
          computedFast = FastCellNormals<double>(pd, polys, numPolys, offsetCells, cellNormals);
        }
      }
      if (!computedFast)
      {
        vtkSMPTools::For(0, numPolys,
          [&](vtkIdType begin, vtkIdType end)
          {
            auto tempCellPointIds = tlTempCellPointIds.Local();
            vtkIdType npts;
            const vtkIdType* pts = nullptr;
            double n[3];
            for (vtkIdType polyId = begin; polyId < end; polyId++)
            {
              polys->GetCellAtId(polyId, npts, pts, tempCellPointIds);
              vtkPolygon::ComputeNormal(points, npts, pts, n);
              cellNormals->SetTuple(offsetCells + polyId, n);
            }
          });
      }

      // Set default value for strip cell normals
      offsetCells += numPolys;
      vtkSMPTools::For(0, numStrips,
        [&](vtkIdType begin, vtkIdType end)
        {
          static constexpr float n[3] = { 1.0, 0.0, 0.0 };
          for (vtkIdType cellId = begin; cellId < end; cellId++)
          {
            // add a default value for strips
            // normals do not have meaningful values, we set them to X
            cellNormals->SetTypedTuple(cellId, n);
          }
        });
    });
  return cellNormals;
}

//-----------------------------------------------------------------------------
vtkSmartPointer<vtkFloatArray> vtkPolyDataNormals::GetPointNormals(
  vtkPolyData* data, vtkFloatArray* cellNormals, double flipDirection)
{
  if (auto existingPointNormals = vtkFloatArray::FastDownCast(data->GetPointData()->GetNormals()))
  {
    return existingPointNormals;
  }
  const vtkIdType numPoints = data->GetNumberOfPoints();
  // check if the cells are already built
  if (data->NeedToBuildCells())
  {
    data->BuildCells();
  }
  data->BuildLinks();

  auto pointNormals = vtkSmartPointer<vtkFloatArray>::New();
  pointNormals->SetName("Normals");
  pointNormals->SetNumberOfComponents(3);
  pointNormals->SetNumberOfTuples(numPoints);
  float* pointNormalsPtr = pointNormals->GetPointer(0);
  float* cellNormalsPtr = cellNormals->GetPointer(0);

  // fvtk: this For writes pointNormals[pointId] = f(pointId) into a pre-sized
  // slot; each point's sum is over its own fixed cell list in a fixed order, so
  // the result is bit-exact under any thread count -> opt into the fvtk
  // default-on multithreading (capped at 4, overridable via VTK SMP APIs).
  fvtk::RunSafeFilterParallel(
    [&]()
    {
      vtkSMPTools::For(0, numPoints,
        [&](vtkIdType begin, vtkIdType end)
        {
          vtkIdType nCells;
          vtkIdType* cells = nullptr;
          for (vtkIdType pointId = begin; pointId < end; pointId++)
          {
            // Initialize point normals
            float* pointNormal = &pointNormalsPtr[3 * pointId];
            pointNormal[0] = pointNormal[1] = pointNormal[2] = 0.0;
            // Compute Point Normals
            data->GetPointCells(pointId, nCells, cells);
            for (vtkIdType i = 0; i < nCells; ++i)
            {
              vtkMath::Add(pointNormal, &cellNormalsPtr[3 * cells[i]], pointNormal);
            }
            // Normalize normals
            const double length = vtkMath::Norm(pointNormal) * flipDirection;
            if (length != 0.0)
            {
              vtkMath::MultiplyScalar(pointNormal, 1.0 / length);
            }
          }
        });
    });

  return pointNormals;
}

//-----------------------------------------------------------------------------
// Generate normals for polygon meshes
int vtkPolyDataNormals::RequestData(vtkInformation* vtkNotUsed(request),
  vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  // get the input and output
  vtkPolyData* input = vtkPolyData::GetData(inputVector[0]);
  vtkPolyData* output = vtkPolyData::GetData(outputVector);

  vtkDebugMacro(<< "Generating surface normals");

  const vtkIdType numInPoints = input->GetNumberOfPoints();
  const vtkIdType numInPolys = input->GetNumberOfPolys();
  const vtkIdType numInStrips = input->GetNumberOfStrips();
  if (numInPoints < 1)
  {
    vtkDebugMacro(<< "No data to generate normals for!");
    return 1;
  }

  // If there is nothing to do, pass the data through
  if ((this->ComputePointNormals == 0 && this->ComputeCellNormals == 0) ||
    (numInPolys < 1 && numInStrips < 1))
  {
    // don't do anything! pass data through
    output->CopyStructure(input);
    output->GetPointData()->PassData(input->GetPointData());
    output->GetCellData()->PassData(input->GetCellData());
    return 1;
  }
  // If the input has normals, and no orientation or splitting is asked pass the data through
  auto inputPointNormals = vtkFloatArray::FastDownCast(input->GetPointData()->GetNormals());
  const bool hasPointNormals = this->ComputePointNormals ? inputPointNormals != nullptr : true;
  auto inputCellNormals = vtkFloatArray::FastDownCast(input->GetCellData()->GetNormals());
  const bool hasCellNormals = this->ComputeCellNormals ? inputCellNormals != nullptr : true;
  if (hasPointNormals && hasCellNormals && (!this->Splitting || !this->ComputePointNormals) &&
    !this->Consistency && !this->AutoOrientNormals)
  {
    // don't do anything! pass data through
    output->CopyStructure(input);
    output->GetPointData()->PassData(input->GetPointData());
    output->GetCellData()->PassData(input->GetCellData());
    return 1;
  }

  // Forwards progress from the all internally used filters to this filter
  vtkNew<vtkEventForwarderCommand> progressForwarder;
  progressForwarder->SetTarget(this);

  vtkNew<vtkTriangleFilter> triangleFilter;
  triangleFilter->SetContainerAlgorithm(this);
  triangleFilter->AddObserver(vtkCommand::ProgressEvent, progressForwarder);
  triangleFilter->SetPassLines(true);
  triangleFilter->SetPassVerts(true);
  triangleFilter->SetPreservePolys(true);
  triangleFilter->SetInputData(input);
  vtkAlgorithmOutput* fixPolyDataPipeline = triangleFilter->GetOutputPort();
  vtkNew<vtkOrientPolyData> orientPolyData;
  if (this->Consistency || this->AutoOrientNormals)
  {
    orientPolyData->SetContainerAlgorithm(this);
    orientPolyData->AddObserver(vtkCommand::ProgressEvent, progressForwarder);
    orientPolyData->SetConsistency(this->Consistency);
    orientPolyData->SetFlipNormals(this->FlipNormals);
    orientPolyData->SetNonManifoldTraversal(this->NonManifoldTraversal);
    orientPolyData->SetAutoOrientNormals(this->AutoOrientNormals);
    orientPolyData->SetInputConnection(fixPolyDataPipeline);
    fixPolyDataPipeline = orientPolyData->GetOutputPort();
  }
  vtkNew<vtkSplitSharpEdgesPolyData> splitSharpEdgesPolyData;
  // splitting is only required if we are computing point normals
  if (this->Splitting && this->ComputePointNormals)
  {
    splitSharpEdgesPolyData->SetContainerAlgorithm(this);
    splitSharpEdgesPolyData->AddObserver(vtkCommand::ProgressEvent, progressForwarder);
    splitSharpEdgesPolyData->SetFeatureAngle(this->FeatureAngle);
    splitSharpEdgesPolyData->SetOutputPointsPrecision(this->OutputPointsPrecision);
    splitSharpEdgesPolyData->SetInputConnection(fixPolyDataPipeline);
    fixPolyDataPipeline = splitSharpEdgesPolyData->GetOutputPort();
  }
  auto fixPolyData = fixPolyDataPipeline->GetProducer();
  fixPolyData->Update();
  output->ShallowCopy(fixPolyData->GetOutputDataObject(0));

  vtkSmartPointer<vtkFloatArray> cellNormals = vtkPolyDataNormals::GetCellNormals(output);
  if (this->ComputeCellNormals)
  {
    output->GetCellData()->SetNormals(cellNormals);
  }
  this->UpdateProgress(0.5);
  if (this->CheckAbort())
  {
    return 1;
  }
  if (this->ComputePointNormals)
  {
    const double flipDirection = this->FlipNormals && !this->Consistency ? -1.0 : 1.0;
    vtkSmartPointer<vtkFloatArray> pointNormals =
      vtkPolyDataNormals::GetPointNormals(output, cellNormals, flipDirection);
    output->GetPointData()->SetNormals(pointNormals);
  }
  // if normals were not requested, they were not part of the input, but are part of the output.
  // remove them
  if (!this->ComputeCellNormals && !input->GetCellData()->GetNormals())
  {
    output->GetCellData()->SetNormals(nullptr);
  }
  if (!this->ComputePointNormals && !input->GetPointData()->GetNormals())
  {
    output->GetPointData()->SetNormals(nullptr);
  }
  // No longer need the links, so free them
  output->SetLinks(nullptr);
  this->UpdateProgress(1.0);

  return 1;
}
VTK_ABI_NAMESPACE_END
