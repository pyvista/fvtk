// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
/**
 * @class   vtkSDKExamplePassThrough
 * @brief   Trivial pass-through filter used to validate the fvtk-sdk.
 *
 * vtkSDKExamplePassThrough is a deliberately minimal vtkPolyDataAlgorithm
 * subclass: it shallow-copies its input vtkPolyData to its output and exposes a
 * single integer Value property. It carries no real functionality; its only
 * purpose is to prove that an out-of-tree VTK module can be both compiled in C++
 * and Python-wrapped against the fvtk-sdk wheel.
 */

#ifndef vtkSDKExamplePassThrough_h
#define vtkSDKExamplePassThrough_h

#include "SDKExampleModule.h" // for export macro
#include "vtkPolyDataAlgorithm.h"

class SDKEXAMPLE_EXPORT vtkSDKExamplePassThrough : public vtkPolyDataAlgorithm
{
public:
  static vtkSDKExamplePassThrough* New();
  vtkTypeMacro(vtkSDKExamplePassThrough, vtkPolyDataAlgorithm);
  void PrintSelf(ostream& os, vtkIndent indent) override;

  ///@{
  /**
   * Get/Set an arbitrary integer value. Exists solely so the Python smoke test
   * has a wrapped accessor pair to call.
   */
  vtkSetMacro(Value, int);
  vtkGetMacro(Value, int);
  ///@}

protected:
  vtkSDKExamplePassThrough();
  ~vtkSDKExamplePassThrough() override;

  int RequestData(vtkInformation* request, vtkInformationVector** inputVector,
    vtkInformationVector* outputVector) override;

  int Value = 0;

private:
  vtkSDKExamplePassThrough(const vtkSDKExamplePassThrough&) = delete;
  void operator=(const vtkSDKExamplePassThrough&) = delete;
};

#endif
