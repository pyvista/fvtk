// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-FileCopyrightText: Copyright (c) Sandia Corporation
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkImageDataToPointSet.h"

#include "vtkCellData.h"
#include "vtkImageData.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMatrix4x4.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkStructuredGrid.h"
#include "vtkStructuredPointArray.h"

#include "vtkNew.h"

VTK_ABI_NAMESPACE_BEGIN
vtkStandardNewMacro(vtkImageDataToPointSet);

//------------------------------------------------------------------------------
vtkImageDataToPointSet::vtkImageDataToPointSet() = default;

vtkImageDataToPointSet::~vtkImageDataToPointSet() = default;

void vtkImageDataToPointSet::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}

//------------------------------------------------------------------------------
int vtkImageDataToPointSet::FillInputPortInformation(int port, vtkInformation* info)
{
  if (!this->Superclass::FillInputPortInformation(port, info))
  {
    return 0;
  }
  info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkImageData");
  return 1;
}

//------------------------------------------------------------------------------
int vtkImageDataToPointSet::RequestData(vtkInformation* vtkNotUsed(request),
  vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  // Retrieve input and output
  vtkImageData* inData = vtkImageData::GetData(inputVector[0]);
  vtkStructuredGrid* outData = vtkStructuredGrid::GetData(outputVector);

  if (inData == nullptr)
  {
    vtkErrorMacro(<< "Input data is nullptr.");
    return 0;
  }
  if (outData == nullptr)
  {
    vtkErrorMacro(<< "Output data is nullptr.");
    return 0;
  }

  // Copy input point and cell data to output
  outData->GetPointData()->PassData(inData->GetPointData());
  outData->GetCellData()->PassData(inData->GetCellData());

  // Extract points coordinates from the image
  vtkIdType nbPoints = inData->GetNumberOfPoints();
  vtkNew<vtkPoints> points;
  points->SetDataTypeToDouble();
  points->SetNumberOfPoints(nbPoints);
  // The output point array is a contiguous double[3] buffer and the input image's
  // coordinates come from an implicit vtkStructuredPointArray<double> backend.
  // Hoist the backend cast out of the loop (vtkImageData::GetPoint re-fetches and
  // re-casts it on every call) and write each coordinate straight into the output
  // buffer at its fixed index 3*i via the same GetTypedTuple() the virtual GetPoint
  // dispatches to. Same math, same values, same fixed indices, same serial order.
  // The abort check is batched to every 4096 points (CheckAbort only sets the abort
  // flag; it never affects output values or order) instead of paying a virtual call
  // per point.
  auto* inPoints = static_cast<vtkStructuredPointArray<double>*>(inData->GetPoints()->GetData());
  auto* outPtr = static_cast<double*>(points->GetVoidPointer(0));
  for (vtkIdType i = 0; i < nbPoints; i++)
  {
    if ((i % 4096 == 0) && this->CheckAbort())
    {
      break;
    }
    inPoints->GetTypedTuple(i, outPtr + 3 * i);
  }
  outData->SetPoints(points);

  // Copy Extent
  int extent[6];
  inData->GetExtent(extent);
  outData->SetExtent(extent);

  return 1;
}
VTK_ABI_NAMESPACE_END
