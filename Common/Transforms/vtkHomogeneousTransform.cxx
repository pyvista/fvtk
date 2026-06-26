// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkHomogeneousTransform.h"

#include "vtkDataArray.h"
#include "vtkDoubleArray.h"
#include "vtkCVISTASMPDefaults.h" // cvista: opt into default multithreading (bit-exact)
#include "vtkFloatArray.h"
#include "vtkMath.h"
#include "vtkMatrix4x4.h"
#include "vtkPoints.h"
#include "vtkSMPTools.h"

#include <vector> // for std::vector

VTK_ABI_NAMESPACE_BEGIN
namespace
{
// cvista devirtualized AOS gather/scatter for the homogeneous (perspective) point
// loop. The per-point homogeneous-divide math (vtkHomogeneousTransformPoint and
// the in-loop vector/normal homogeneous correction) is unchanged; only the
// virtual vtkPoints::GetPoint/SetPoint and vtkDataArray::GetTuple/SetTuple that
// wrap it are resolved to raw contiguous AOS float/double pointers ONCE.
//
// Bit-exactness: GetTuple into a double[3] is dst[c]=ptr[3*id+c] for AOS double
// and dst[c]=static_cast<double>(ptr[3*id+c]) for AOS float (the identical
// widening); SetTuple from a double[3] is ptr[3*id+c]=src[c] for AOS double and
// ptr[3*id+c]=static_cast<float>(src[c]) for AOS float (the identical narrowing).
// The transform math therefore sees byte-identical inputs and writes
// byte-identical outputs in the identical loop order. Non-AOS storage keeps the
// cached pointer null and falls back to the original virtual path.
class HomogTransformAOSReader
{
public:
  explicit HomogTransformAOSReader(vtkDataArray* arr)
  {
    if (arr)
    {
      if (auto* da = vtkDoubleArray::FastDownCast(arr))
      {
        this->DPtr = da->GetPointer(0);
      }
      else if (auto* fa = vtkFloatArray::FastDownCast(arr))
      {
        this->FPtr = fa->GetPointer(0);
      }
      this->Fallback = arr;
    }
  }

  inline void Read(vtkIdType id, double x[3]) const
  {
    if (this->DPtr)
    {
      const double* p = this->DPtr + 3 * id;
      x[0] = p[0];
      x[1] = p[1];
      x[2] = p[2];
    }
    else if (this->FPtr)
    {
      const float* p = this->FPtr + 3 * id;
      x[0] = static_cast<double>(p[0]);
      x[1] = static_cast<double>(p[1]);
      x[2] = static_cast<double>(p[2]);
    }
    else
    {
      this->Fallback->GetTuple(id, x);
    }
  }

private:
  const double* DPtr = nullptr;
  const float* FPtr = nullptr;
  vtkDataArray* Fallback = nullptr;
};

class HomogTransformAOSWriter
{
public:
  explicit HomogTransformAOSWriter(vtkDataArray* arr)
  {
    if (arr)
    {
      if (auto* da = vtkDoubleArray::FastDownCast(arr))
      {
        this->DPtr = da->GetPointer(0);
      }
      else if (auto* fa = vtkFloatArray::FastDownCast(arr))
      {
        this->FPtr = fa->GetPointer(0);
      }
      this->Fallback = arr;
    }
  }

  inline void Write(vtkIdType id, const double x[3]) const
  {
    if (this->DPtr)
    {
      double* p = this->DPtr + 3 * id;
      p[0] = x[0];
      p[1] = x[1];
      p[2] = x[2];
    }
    else if (this->FPtr)
    {
      float* p = this->FPtr + 3 * id;
      p[0] = static_cast<float>(x[0]);
      p[1] = static_cast<float>(x[1]);
      p[2] = static_cast<float>(x[2]);
    }
    else
    {
      this->Fallback->SetTuple(id, x);
    }
  }

private:
  double* DPtr = nullptr;
  float* FPtr = nullptr;
  vtkDataArray* Fallback = nullptr;
};

void TransformVector(double M[4][4], double* outPnt, double f, double* inVec, double* outVec)
{
  // do the linear homogeneous transformation
  outVec[0] = M[0][0] * inVec[0] + M[0][1] * inVec[1] + M[0][2] * inVec[2];
  outVec[1] = M[1][0] * inVec[0] + M[1][1] * inVec[1] + M[1][2] * inVec[2];
  outVec[2] = M[2][0] * inVec[0] + M[2][1] * inVec[1] + M[2][2] * inVec[2];
  double w = M[3][0] * inVec[0] + M[3][1] * inVec[1] + M[3][2] * inVec[2];

  // apply homogeneous correction: note that the f we are using
  // is the one we calculated in the point transformation
  outVec[0] = (outVec[0] - w * outPnt[0]) * f;
  outVec[1] = (outVec[1] - w * outPnt[1]) * f;
  outVec[2] = (outVec[2] - w * outPnt[2]) * f;
}
}
//------------------------------------------------------------------------------
vtkHomogeneousTransform::vtkHomogeneousTransform()
{
  this->Matrix = vtkMatrix4x4::New();
}

//------------------------------------------------------------------------------
vtkHomogeneousTransform::~vtkHomogeneousTransform()
{
  if (this->Matrix)
  {
    this->Matrix->Delete();
  }
}

//------------------------------------------------------------------------------
void vtkHomogeneousTransform::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  os << indent << "Matrix: (" << this->Matrix << ")\n";
  if (this->Matrix)
  {
    this->Matrix->PrintSelf(os, indent.GetNextIndent());
  }
}

//------------------------------------------------------------------------------
template <class T1, class T2, class T3>
inline double vtkHomogeneousTransformPoint(T1 M[4][4], T2 in[3], T3 out[3])
{
  double x = M[0][0] * in[0] + M[0][1] * in[1] + M[0][2] * in[2] + M[0][3];
  double y = M[1][0] * in[0] + M[1][1] * in[1] + M[1][2] * in[2] + M[1][3];
  double z = M[2][0] * in[0] + M[2][1] * in[1] + M[2][2] * in[2] + M[2][3];
  double w = M[3][0] * in[0] + M[3][1] * in[1] + M[3][2] * in[2] + M[3][3];

  double f = 1.0 / w;
  out[0] = static_cast<T3>(x * f);
  out[1] = static_cast<T3>(y * f);
  out[2] = static_cast<T3>(z * f);

  return f;
}

//------------------------------------------------------------------------------
// computes a coordinate transformation and also returns the Jacobian matrix
template <class T1, class T2, class T3, class T4>
inline void vtkHomogeneousTransformDerivative(T1 M[4][4], T2 in[3], T3 out[3], T4 derivative[3][3])
{
  double f = vtkHomogeneousTransformPoint(M, in, out);

  for (int i = 0; i < 3; i++)
  {
    derivative[0][i] = static_cast<T4>((M[0][i] - M[3][i] * out[0]) * f);
    derivative[1][i] = static_cast<T4>((M[1][i] - M[3][i] * out[1]) * f);
    derivative[2][i] = static_cast<T4>((M[2][i] - M[3][i] * out[2]) * f);
  }
}

//------------------------------------------------------------------------------
void vtkHomogeneousTransform::InternalTransformPoint(const float in[3], float out[3])
{
  vtkHomogeneousTransformPoint(this->Matrix->Element, in, out);
}

//------------------------------------------------------------------------------
void vtkHomogeneousTransform::InternalTransformPoint(const double in[3], double out[3])
{
  vtkHomogeneousTransformPoint(this->Matrix->Element, in, out);
}

//------------------------------------------------------------------------------
void vtkHomogeneousTransform::InternalTransformDerivative(
  const float in[3], float out[3], float derivative[3][3])
{
  vtkHomogeneousTransformDerivative(this->Matrix->Element, in, out, derivative);
}

//------------------------------------------------------------------------------
void vtkHomogeneousTransform::InternalTransformDerivative(
  const double in[3], double out[3], double derivative[3][3])
{
  vtkHomogeneousTransformDerivative(this->Matrix->Element, in, out, derivative);
}

//------------------------------------------------------------------------------
void vtkHomogeneousTransform::TransformPoints(vtkPoints* inPts, vtkPoints* outPts)
{
  vtkIdType n = inPts->GetNumberOfPoints();
  vtkIdType m = outPts->GetNumberOfPoints();
  outPts->SetNumberOfPoints(m + n);
  double(*M)[4] = this->Matrix->Element;

  this->Update();

  // cvista: devirtualize coordinate gather/scatter; same per-point homogeneous math.
  const HomogTransformAOSReader inReader(inPts->GetData());
  const HomogTransformAOSWriter outWriter(outPts->GetData());

  // cvista: per-point-independent writes to a pre-sized output => bit-exact under
  // any thread count; run under the default-threading policy.
  cvista::RunSafeFilterParallel(
    [&]()
    {
      vtkSMPTools::For(0, n, vtkSMPTools::THRESHOLD,
        [&](vtkIdType ptId, vtkIdType endPtId)
        {
          double point[3];
          for (; ptId < endPtId; ++ptId)
          {
            inReader.Read(ptId, point);
            vtkHomogeneousTransformPoint(M, point, point);
            outWriter.Write(m + ptId, point);
          }
        });
    });
}

//------------------------------------------------------------------------------
// Transform the normals and vectors using the derivative of the
// transformation.  Either inNms or inVrs can be set to nullptr.
// Normals are multiplied by the inverse transpose of the transform
// derivative, while vectors are simply multiplied by the derivative.
// Note that the derivative of the inverse transform is simply the
// inverse of the derivative of the forward transform.
void vtkHomogeneousTransform::TransformPointsNormalsVectors(vtkPoints* inPts, vtkPoints* outPts,
  vtkDataArray* inNms, vtkDataArray* outNms, vtkDataArray* inVrs, vtkDataArray* outVrs,
  int nOptionalVectors, vtkDataArray** inVrsArr, vtkDataArray** outVrsArr)
{
  double(*M)[4] = this->Matrix->Element;
  double L[4][4];

  this->Update();

  vtkIdType n = inPts->GetNumberOfPoints();
  vtkIdType m = outPts->GetNumberOfPoints();
  outPts->SetNumberOfPoints(m + n);
  if (inVrs)
  {
    outVrs->SetNumberOfTuples(m + n);
  }
  if (inVrsArr)
  {
    for (int iArr = 0; iArr < nOptionalVectors; iArr++)
    {
      outVrsArr[iArr]->SetNumberOfTuples(m + n);
    }
  }
  if (inNms)
  {
    outNms->SetNumberOfTuples(m + n);
    // need inverse of the matrix to calculate normals
    vtkMatrix4x4::DeepCopy(*L, this->Matrix);
    vtkMatrix4x4::Invert(*L, *L);
    vtkMatrix4x4::Transpose(*L, *L);
  }

  // cvista: devirtualize coordinate / vector / normal gather/scatter; same per-point
  // homogeneous math.
  const HomogTransformAOSReader inPtReader(inPts->GetData());
  const HomogTransformAOSWriter outPtWriter(outPts->GetData());
  const HomogTransformAOSReader inVrsReader(inVrs);
  const HomogTransformAOSWriter outVrsWriter(outVrs);
  const HomogTransformAOSReader inNmsReader(inNms);
  const HomogTransformAOSWriter outNmsWriter(outNms);
  std::vector<HomogTransformAOSReader> inVrsArrReaders;
  std::vector<HomogTransformAOSWriter> outVrsArrWriters;
  if (inVrsArr)
  {
    inVrsArrReaders.reserve(nOptionalVectors);
    outVrsArrWriters.reserve(nOptionalVectors);
    for (int iArr = 0; iArr < nOptionalVectors; iArr++)
    {
      inVrsArrReaders.emplace_back(inVrsArr[iArr]);
      outVrsArrWriters.emplace_back(outVrsArr[iArr]);
    }
  }

  // cvista: per-point-independent writes to pre-sized outputs => bit-exact under
  // any thread count; run under the default-threading policy.
  cvista::RunSafeFilterParallel([&]() {
  vtkSMPTools::For(0, n, vtkSMPTools::THRESHOLD,
    [&](vtkIdType ptId, vtkIdType endPtId)
    {
      double inPnt[3], outPnt[3], inNrm[3], outNrm[3], inVec[3], outVec[3];
      for (; ptId < endPtId; ++ptId)
      {
        inPtReader.Read(ptId, inPnt);

        // do the coordinate transformation, get 1/w
        double f = vtkHomogeneousTransformPoint(M, inPnt, outPnt);
        outPtWriter.Write(m + ptId, outPnt);

        if (inVrs)
        {
          inVrsReader.Read(ptId, inVec);
          TransformVector(M, outPnt, f, inVec, outVec);
          outVrsWriter.Write(m + ptId, outVec);
        }

        if (inVrsArr)
        {
          for (int iArr = 0; iArr < nOptionalVectors; iArr++)
          {
            inVrsArrReaders[iArr].Read(ptId, inVec);
            TransformVector(M, outPnt, f, inVec, outVec);
            outVrsArrWriters[iArr].Write(m + ptId, outVec);
          }
        }

        if (inNms)
        {
          inNmsReader.Read(ptId, inNrm);

          // calculate the w component of the normal
          double w = -(inNrm[0] * inPnt[0] + inNrm[1] * inPnt[1] + inNrm[2] * inPnt[2]);

          // perform the transformation in homogeneous coordinates
          outNrm[0] = L[0][0] * inNrm[0] + L[0][1] * inNrm[1] + L[0][2] * inNrm[2] + L[0][3] * w;
          outNrm[1] = L[1][0] * inNrm[0] + L[1][1] * inNrm[1] + L[1][2] * inNrm[2] + L[1][3] * w;
          outNrm[2] = L[2][0] * inNrm[0] + L[2][1] * inNrm[1] + L[2][2] * inNrm[2] + L[2][3] * w;

          // re-normalize
          vtkMath::Normalize(outNrm);
          outNmsWriter.Write(m + ptId, outNrm);
        }
      }
    });
  }); // cvista: end RunSafeFilterParallel
}

//------------------------------------------------------------------------------
// update and copy out the current matrix
void vtkHomogeneousTransform::GetMatrix(vtkMatrix4x4* m)
{
  this->Update();
  m->DeepCopy(this->Matrix);
}

//------------------------------------------------------------------------------
void vtkHomogeneousTransform::InternalDeepCopy(vtkAbstractTransform* transform)
{
  vtkHomogeneousTransform* t = static_cast<vtkHomogeneousTransform*>(transform);

  this->Matrix->DeepCopy(t->Matrix);
}
VTK_ABI_NAMESPACE_END
