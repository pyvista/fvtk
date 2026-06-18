// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkWarpVector.h"

#include "vtkArrayDispatch.h"
#include "vtkArrayDispatchDataSetArrayList.h"
#include "vtkCellData.h"
#include "vtkDataArray.h"
#include "vtkDataArrayRange.h"
#include "vtkImageData.h"
#include "vtkImageDataToPointSet.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkPointSet.h"
#include "vtkPoints.h"
#include "vtkRectilinearGrid.h"
#include "vtkRectilinearGridToPointSet.h"
#include "vtkStructuredGrid.h"

#include "vtkFVTKSMPDefaults.h"
#include "vtkNew.h"
#include "vtkSMPTools.h"
#include "vtkSmartPointer.h"

#include <algorithm> // std::min for fvtk SIMD batch bounds

VTK_ABI_NAMESPACE_BEGIN
vtkStandardNewMacro(vtkWarpVector);

//------------------------------------------------------------------------------
vtkWarpVector::vtkWarpVector()
{
  this->ScaleFactor = 1.0;
  this->OutputPointsPrecision = vtkAlgorithm::DEFAULT_PRECISION;

  // by default process active point vectors
  this->SetInputArrayToProcess(
    0, 0, 0, vtkDataObject::FIELD_ASSOCIATION_POINTS, vtkDataSetAttributes::VECTORS);
}

//------------------------------------------------------------------------------
vtkWarpVector::~vtkWarpVector() = default;

//------------------------------------------------------------------------------
int vtkWarpVector::FillInputPortInformation(int vtkNotUsed(port), vtkInformation* info)
{
  info->Remove(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE());
  info->Append(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkPointSet");
  info->Append(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkImageData");
  info->Append(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkRectilinearGrid");
  return 1;
}

//------------------------------------------------------------------------------
int vtkWarpVector::RequestDataObject(
  vtkInformation* request, vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  vtkImageData* inImage = vtkImageData::GetData(inputVector[0]);
  vtkRectilinearGrid* inRect = vtkRectilinearGrid::GetData(inputVector[0]);

  if (inImage || inRect)
  {
    vtkStructuredGrid* output = vtkStructuredGrid::GetData(outputVector);
    if (!output)
    {
      vtkNew<vtkStructuredGrid> newOutput;
      outputVector->GetInformationObject(0)->Set(vtkDataObject::DATA_OBJECT(), newOutput);
    }
    return 1;
  }
  else
  {
    return this->Superclass::RequestDataObject(request, inputVector, outputVector);
  }
}

//------------------------------------------------------------------------------
// Core methods to scale points with vectors
namespace
{ // anonymous

// fvtk SIMD: the per-point warp kernel xo = xi + sf*v over a contiguous
// [ptId, endPtId) range, hoisted out of the abort-checked loop into a dedicated
// free function carrying target_clones("default","avx2"). The per-element
// CheckAbort/GetAbortOutput branches (virtual calls) blocked vectorization, so
// the abort check is moved to a coarse outer batch (see operator() below) and
// this clean inner kernel multi-versions to a baseline (SSE2) clone + an .avx2
// clone + IFUNC resolver — single portable wheel, no -march bump. BIT-EXACT:
// xi + sf*v is the canonical a*b+c FMA shape; the TU is compiled with
// -ffp-contract=off (set_source_files_properties in CMakeLists) so no clone
// contracts to vfmadd, holding maxULP=0 vs stock VTK. NB: warp is
// bandwidth-bound (2 read + 1 write per ~2 FLOP) so the SIMD win is real only
// cache-resident; the FMV stays portable/bit-exact either way.
// fvtk PORTABILITY: target_clones("default","avx2") is GCC x86-only function
// multiversioning; AppleClang (Apple Silicon) and MSVC reject it. Guard to real
// GCC-on-x86 and no-op elsewhere (those targets compile the same bit-exact
// baseline kernel without the AVX2 clone). See vtkLinearTransform.cxx.
#if defined(__GNUC__) && !defined(__clang__) && (defined(__x86_64__) || defined(__i386__))
#define FVTK_AVX2_TARGET_CLONES __attribute__((target_clones("default", "avx2")))
#else
#define FVTK_AVX2_TARGET_CLONES
#endif
template <typename IptsRange, typename OptsRange, typename VecsRange>
FVTK_AVX2_TARGET_CLONES void fvtkWarpVectorRange(
  const IptsRange& ipts, OptsRange& opts, const VecsRange& vecs, double sf, vtkIdType begin,
  vtkIdType end)
{
  for (vtkIdType ptId = begin; ptId < end; ++ptId)
  {
    const auto xi = ipts[ptId];
    auto xo = opts[ptId];
    const auto v = vecs[ptId];

    xo[0] = xi[0] + sf * v[0];
    xo[1] = xi[1] + sf * v[1];
    xo[2] = xi[2] + sf * v[2];
  }
}

struct WarpWorker
{
  template <typename InPT, typename OutPT, typename VT>
  void operator()(InPT* inPts, OutPT* outPts, VT* vectors, vtkWarpVector* self, double sf)

  {
    vtkIdType numPts = inPts->GetNumberOfTuples();
    const auto ipts = vtk::DataArrayTupleRange<3>(inPts);
    auto opts = vtk::DataArrayTupleRange<3>(outPts);
    const auto vecs = vtk::DataArrayTupleRange<3>(vectors);

    // We use THRESHOLD to test if the data size is small enough
    // to execute the functor serially.
    // fvtk: this For writes opts[ptId] = f(ipts[ptId]) into pre-sized output
    // slots, so it is bit-exact under any thread count -> opt into the fvtk
    // default-on multithreading (capped at 4, overridable via VTK SMP APIs).
    fvtk::RunSafeFilterParallel(
      [&]()
      {
        vtkSMPTools::For(0, numPts, vtkSMPTools::THRESHOLD,
          [&](vtkIdType ptId, vtkIdType endPtId)
          {
            bool isFirst = vtkSMPTools::GetSingleThread();
            // fvtk: process in batches so the per-point abort branch is lifted
            // out of the vectorizable kernel (fvtkWarpVectorRange, AVX2/SSE2
            // multi-versioned). The output is identical to a per-point check:
            // CheckAbort only sets a flag; GetAbortOutput breaks the chunk loop
            // at a batch boundary, and on abort the (undefined) tail is
            // discarded exactly as before.
            constexpr vtkIdType kBatch = 4096;
            for (vtkIdType base = ptId; base < endPtId; base += kBatch)
            {
              if (isFirst)
              {
                self->CheckAbort();
              }
              if (self->GetAbortOutput())
              {
                break;
              }
              const vtkIdType batchEnd = std::min(base + kBatch, endPtId);
              fvtkWarpVectorRange(ipts, opts, vecs, sf, base, batchEnd);
            }
          }); // lambda
      });
  }
};

} // anonymous namespace

//------------------------------------------------------------------------------
int vtkWarpVector::RequestData(vtkInformation* vtkNotUsed(request),
  vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  vtkSmartPointer<vtkPointSet> input = vtkPointSet::GetData(inputVector[0]);
  vtkPointSet* output = vtkPointSet::GetData(outputVector);

  if (!input)
  {
    // Try converting image data.
    vtkImageData* inImage = vtkImageData::GetData(inputVector[0]);
    if (inImage)
    {
      vtkNew<vtkImageDataToPointSet> image2points;
      image2points->SetInputData(inImage);
      image2points->SetContainerAlgorithm(this);
      image2points->Update();
      input = image2points->GetOutput();
    }
  }

  if (!input)
  {
    // Try converting rectilinear grid.
    vtkRectilinearGrid* inRect = vtkRectilinearGrid::GetData(inputVector[0]);
    if (inRect)
    {
      vtkNew<vtkRectilinearGridToPointSet> rect2points;
      rect2points->SetInputData(inRect);
      rect2points->SetContainerAlgorithm(this);
      rect2points->Update();
      input = rect2points->GetOutput();
    }
  }

  if (!input)
  {
    vtkErrorMacro(<< "Invalid or missing input");
    return 0;
  }

  // First, copy the input to the output as a starting point
  output->CopyStructure(input);

  vtkPoints* inPts;
  if (input == nullptr || (inPts = input->GetPoints()) == nullptr)
  {
    return 1;
  }
  vtkIdType numPts = inPts->GetNumberOfPoints();
  vtkDataArray* vectors = this->GetInputArrayToProcess(0, inputVector);

  if (!vectors || !numPts)
  {
    vtkDebugMacro(<< "No input data");
    return 1;
  }

  // Create the output points. By default, the output type is the
  // same as the input type.
  vtkNew<vtkPoints> newPts;
  if (this->OutputPointsPrecision == vtkAlgorithm::DEFAULT_PRECISION)
  {
    newPts->SetDataType(inPts->GetDataType());
  }
  else if (this->OutputPointsPrecision == vtkAlgorithm::SINGLE_PRECISION)
  {
    newPts->SetDataType(VTK_FLOAT);
  }
  else
  {
    newPts->SetDataType(VTK_DOUBLE);
  }
  newPts->SetNumberOfPoints(numPts);
  output->SetPoints(newPts);

  assert(vectors->GetNumberOfComponents() == 3);
  assert(inPts->GetData()->GetNumberOfComponents() == 3);
  assert(newPts->GetData()->GetNumberOfComponents() == 3);

  // Dispatch over point and scalar types. Fastpath for real types, fallback to slower
  // path for non-real types.
  using FloatArrays = vtkArrayDispatch::FilterArraysByValueType<vtkArrayDispatch::Arrays,
    vtkArrayDispatch::Reals>::Result;
  using WarpDispatch = vtkArrayDispatch::Dispatch3ByArray<vtkArrayDispatch::PointArrays,
    vtkArrayDispatch::AOSPointArrays, FloatArrays>;
  WarpWorker warpWorker;

  if (!WarpDispatch::Execute(
        inPts->GetData(), newPts->GetData(), vectors, warpWorker, this, this->ScaleFactor))
  { // fallback to slowpath
    warpWorker(inPts->GetData(), newPts->GetData(), vectors, this, this->ScaleFactor);
  }

  // now pass the data.
  output->GetPointData()->CopyNormalsOff(); // distorted geometry
  output->GetPointData()->PassData(input->GetPointData());
  output->GetCellData()->PassData(input->GetCellData());

  return 1;
}

//------------------------------------------------------------------------------
void vtkWarpVector::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);

  os << indent << "Scale Factor: " << this->ScaleFactor << "\n";
  os << indent << "Output Points Precision: " << this->OutputPointsPrecision << "\n";
}
VTK_ABI_NAMESPACE_END
