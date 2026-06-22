// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkSDKExamplePassThrough.h"

#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkObjectFactory.h"
#include "vtkPolyData.h"

vtkStandardNewMacro(vtkSDKExamplePassThrough);

vtkSDKExamplePassThrough::vtkSDKExamplePassThrough() = default;

vtkSDKExamplePassThrough::~vtkSDKExamplePassThrough() = default;

void vtkSDKExamplePassThrough::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  os << indent << "Value: " << this->Value << "\n";
}

int vtkSDKExamplePassThrough::RequestData(vtkInformation* vtkNotUsed(request),
  vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  vtkPolyData* input = vtkPolyData::GetData(inputVector[0]);
  vtkPolyData* output = vtkPolyData::GetData(outputVector);

  if (input && output)
  {
    output->ShallowCopy(input);
  }
  return 1;
}
