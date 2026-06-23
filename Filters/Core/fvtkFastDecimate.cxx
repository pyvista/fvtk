// SPDX-License-Identifier: BSD-3-Clause
// fvtk opt-in fast parallel triangle-mesh decimation: the vtkDecimatePro fast
// path. A MIS-wave half-edge-collapse decimator. This TU is excluded from the
// unity build so its VTK_*_VERTEX macros (copied verbatim from
// vtkDecimatePro.cxx) stay isolated.
//
// The per-vertex evaluation math (EvaluateVertex, IsValidSplit, SplitLoop, the
// SIMPLE/BOUNDARY cases of FindSplit) is a mechanical, REENTRANT port of the
// like-named methods in Filters/Core/vtkDecimatePro.cxx. Those methods mutate
// shared this->-scoped scratch (this->V / this->T / this->Neighbors / this->X /
// this->Normal / this->Pt / this->LoopArea), so they are NOT thread-safe; the
// DecimateLocal struct below owns its OWN copies of every scratch buffer so each
// thread evaluates a vertex with zero shared mutable state. Cited line numbers
// refer to Filters/Core/vtkDecimatePro.cxx as of this branch.
#include "fvtkFastDecimate.h"

#include "vtkFVTKSMPDefaults.h" // fvtk::FastModeEnabled / RunFastFilterParallel

#include "vtkAlgorithm.h" // SINGLE_PRECISION / DOUBLE_PRECISION / DEFAULT_PRECISION
#include "vtkCellArray.h"
#include "vtkCellData.h"
#include "vtkIdList.h"
#include "vtkLine.h"
#include "vtkMath.h"
#include "vtkNew.h"
#include "vtkPlane.h"
#include "vtkPointData.h"
#include "vtkPoints.h"
#include "vtkPolyData.h"
#include "vtkPolyDataEdgeNeighbors.h" // FastEdgeNeighbors (thread-safe, reads links fresh)
#include "vtkSMPThreadLocal.h"
#include "vtkSMPTools.h"
#include "vtkSmartPointer.h"
#include "vtkTriangle.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

// Vertex classification codes -- copied verbatim from vtkDecimatePro.cxx L27-35
// so the ported EvaluateVertex returns the identical type codes. Local linkage
// (this TU is SKIP_UNITY so they cannot clash).
#define VTK_SIMPLE_VERTEX 1
#define VTK_BOUNDARY_VERTEX 2
#define VTK_INTERIOR_EDGE_VERTEX 3
#define VTK_CORNER_VERTEX 4
#define VTK_CRACK_TIP_VERTEX 5
#define VTK_EDGE_END_VERTEX 6
#define VTK_NON_MANIFOLD_VERTEX 7
#define VTK_DEGENERATE_VERTEX 8
#define VTK_HIGH_DEGREE_VERTEX 9

// vtkDecimatePro.cxx L23-24.
#define FVTK_DECIM_TOLERANCE 1.0e-05
#define FVTK_DECIM_MAX_TRIS_PER_VERTEX VTK_CELL_SIZE

namespace
{
VTK_ABI_NAMESPACE_BEGIN

// Engagement counter: number of successful fast runs. Exposed through
// fvtk::GetFastDecimateEngageCount() and vtkDecimatePro::GetFastModeEngageCount.
std::atomic<std::uint64_t> g_engageCount{ 0 };

// Helper error functions -- copies of vtkDecimatePro.cxx L433-457.
inline double ComputeEdgeError(double x[3], double x1[3], double x2[3])
{
  double projDist = vtkLine::DistanceToLine(x, x1, x2);
  double edgeLength = vtkMath::Distance2BetweenPoints(x1, x2);
  return (projDist < edgeLength ? projDist : edgeLength);
}
inline double ComputeSingleTriangleError(double x[3], double x1[3], double x2[3])
{
  return vtkTriangle::TriangleArea(x, x1, x2);
}
inline double ComputeSimpleError(double x[3], double normal[3], double point[3])
{
  double dist = vtkPlane::DistanceToPlane(x, normal, point);
  return dist * dist;
}

// Per-vertex link-loop scratch. Mirrors vtkDecimatePro::LocalVertex /
// LocalTri / VertexArray / TriArray (vtkDecimatePro.h L319-384) but with
// fixed-size arrays the DecimateLocal struct owns (no shared this->).
struct LocalVertex
{
  vtkIdType id;
  double x[3];
  double FAngle;
};
struct LocalTri
{
  vtkIdType id;
  double area;
  double n[3];
};

// Result of evaluating one vertex: classification, error, and the collapse
// target / triangle bookkeeping needed to APPLY the half-edge collapse later.
// Deterministic function of the (read-only) mesh state.
struct VertexEval
{
  int vtype = VTK_DEGENERATE_VERTEX;
  double error = VTK_DOUBLE_MAX;
  bool removable = false;
  vtkIdType collapseId = -1; // existing input vertex collapsed ONTO
  vtkIdType pt1 = -1;
  vtkIdType pt2 = -1;
  vtkIdType tri0 = -1; // CollapseTris[0]
  vtkIdType tri1 = -1; // CollapseTris[1] (-1 if numDeleted==1)
  int numDeleted = 0;  // 2 (SIMPLE) or 1 (BOUNDARY)
};

// Reentrant per-thread evaluator. Owns ALL scratch (V/T/Neighbors/X/Normal/Pt/
// LoopArea). Constructed once per thread (vtkSMPThreadLocal); Evaluate() is a
// faithful port of vtkDecimatePro::EvaluateVertex + FindSplit (SIMPLE/BOUNDARY)
// + IsValidSplit + SplitLoop, with every "this->X" replaced by a member of THIS
// struct. No live vtkDecimatePro method is ever called from a thread.
struct DecimateLocal
{
  vtkPolyData* Mesh;
  double CosAngle;
  double Tolerance;     // VTK_TOLERANCE * input->GetLength()  (L169)
  double Error;         // MaximumError-derived cap (L160-168)
  bool BoundaryDeletion;
  vtkPolyDataEdgeNeighbors::FastEdgeNeighbors EdgeNeighbors;

  // Owned scratch (one set per thread).
  std::vector<LocalVertex> V; // V->Array, indices 0..VMaxId
  std::vector<LocalTri> T;    // T->Array, indices 0..TMaxId
  vtkIdType VMaxId = -1;
  vtkIdType TMaxId = -1;
  // vtkSmartPointer (not vtkNew) so the struct stays copy-constructible: it is
  // used as the exemplar of a vtkSMPThreadLocal, which copy-constructs one
  // DecimateLocal PER THREAD. The copy constructor below allocates a FRESH,
  // independent vtkIdList for each thread so no scratch is ever shared.
  vtkSmartPointer<vtkIdList> Neighbors;
  double X[3];
  double Normal[3];
  double Pt[3];
  double LoopArea = 0.0;

  DecimateLocal(vtkPolyData* mesh, double cosAngle, double tol, double err, bool bdel)
    : Mesh(mesh)
    , CosAngle(cosAngle)
    , Tolerance(tol)
    , Error(err)
    , BoundaryDeletion(bdel)
    , EdgeNeighbors(mesh)
  {
    this->V.resize(FVTK_DECIM_MAX_TRIS_PER_VERTEX + 2);
    this->T.resize(FVTK_DECIM_MAX_TRIS_PER_VERTEX + 2);
    this->Neighbors = vtkSmartPointer<vtkIdList>::New();
    this->Neighbors->Allocate(FVTK_DECIM_MAX_TRIS_PER_VERTEX);
  }

  // Copy constructor (used per-thread by vtkSMPThreadLocal). Copies the scalar
  // config + the FastEdgeNeighbors accessor (read-only, re-fetches links per
  // Get()), but gives this copy its OWN scratch buffers and its OWN vtkIdList.
  DecimateLocal(const DecimateLocal& o)
    : Mesh(o.Mesh)
    , CosAngle(o.CosAngle)
    , Tolerance(o.Tolerance)
    , Error(o.Error)
    , BoundaryDeletion(o.BoundaryDeletion)
    , EdgeNeighbors(o.EdgeNeighbors)
  {
    this->V.resize(FVTK_DECIM_MAX_TRIS_PER_VERTEX + 2);
    this->T.resize(FVTK_DECIM_MAX_TRIS_PER_VERTEX + 2);
    this->Neighbors = vtkSmartPointer<vtkIdList>::New();
    this->Neighbors->Allocate(FVTK_DECIM_MAX_TRIS_PER_VERTEX);
  }
  DecimateLocal& operator=(const DecimateLocal&) = delete;

  vtkIdType VCount() const { return this->VMaxId + 1; }
  vtkIdType TCount() const { return this->TMaxId + 1; }

  // ----- Port of vtkDecimatePro::EvaluateVertex (L489-866). Returns the vtype
  // and fills fedges[0..1] like the original (boundary feature edges). On
  // return the V/T/Normal/Pt/LoopArea members hold the ordered link loop.
  int EvaluateVertex(vtkIdType ptId, vtkIdType numTris, const vtkIdType* tris, vtkIdType fedges[2])
  {
    vtkIdType numNei, numFEdges;
    vtkIdType numVerts;
    LocalTri t;
    LocalVertex sn;
    vtkIdType startVertex, nextVertex, numNormals;
    int i, j, vtype;
    const vtkIdType* verts;
    double *x1, *x2, *normal;
    double v1[3], v2[3], center[3];

    // High vertex degree -- L515-518. (Conservatively not-removable.)
    if (numTris >= FVTK_DECIM_MAX_TRIS_PER_VERTEX)
    {
      return VTK_HIGH_DEGREE_VERTEX;
    }

    this->VMaxId = -1;
    this->TMaxId = -1;

    sn.FAngle = 0.0;
    t.area = 0.0;
    t.n[0] = t.n[1] = t.n[2] = 0.0;

    // Find the starting edge (L539-555).
    this->Mesh->GetCellPoints(*tris, numVerts, verts);
    for (i = 0; i < 3; i++)
    {
      if (verts[i] == ptId)
      {
        break;
      }
    }
    sn.id = startVertex = verts[(i + 1) % 3];
    this->Mesh->GetPoint(sn.id, sn.x);
    this->V[++this->VMaxId] = sn;

    nextVertex = -1;
    this->Neighbors->Reset();
    this->Neighbors->InsertId(0, *tris);
    numNei = 1;

    // Traverse the link loop one way (L561-591).
    while (this->TMaxId < numTris && numNei == 1 && nextVertex != startVertex)
    {
      t.id = this->Neighbors->GetId(0);
      this->Mesh->GetCellPoints(t.id, numVerts, verts);
      if (verts[0] == verts[1] || verts[1] == verts[2] || verts[0] == verts[2])
      {
        return VTK_DEGENERATE_VERTEX;
      }
      this->T[++this->TMaxId] = t;
      for (j = 0; j < 3; j++)
      {
        if (verts[j] != sn.id && verts[j] != ptId)
        {
          nextVertex = verts[j];
          break;
        }
      }
      sn.id = nextVertex;
      this->Mesh->GetPoint(sn.id, sn.x);
      this->V[++this->VMaxId] = sn;
      this->EdgeNeighbors.Get(t.id, ptId, nextVertex, this->Neighbors);
      numNei = this->Neighbors->GetNumberOfIds();
    }

    // Classify how the loop closed (L596-719).
    if (nextVertex == startVertex && numNei == 1)
    {
      if (this->TCount() != numTris)
      {
        vtype = VTK_NON_MANIFOLD_VERTEX;
      }
      else
      {
        this->VMaxId -= 1;
        vtype = VTK_SIMPLE_VERTEX;
      }
    }
    else if (numNei > 1 || this->TCount() > numTris)
    {
      vtype = VTK_NON_MANIFOLD_VERTEX;
    }
    else if (numNei == 0 && this->TCount() == numTris)
    {
      this->V[0].FAngle = -1.0;
      this->V[this->VMaxId].FAngle = -1.0;
      vtype = VTK_BOUNDARY_VERTEX;
    }
    else
    {
      // Hit a boundary but didn't complete; go back the other way (L629-718).
      t = this->T[this->TMaxId];
      this->VMaxId = -1;
      this->TMaxId = -1;

      startVertex = sn.id = nextVertex;
      this->Mesh->GetPoint(sn.id, sn.x);
      this->V[++this->VMaxId] = sn;

      nextVertex = -1;
      this->Neighbors->Reset();
      this->Neighbors->InsertId(0, t.id);
      numNei = 1;

      while (this->TMaxId < numTris && numNei == 1 && nextVertex != startVertex)
      {
        t.id = this->Neighbors->GetId(0);
        this->Mesh->GetCellPoints(t.id, numVerts, verts);
        if (verts[0] == verts[1] || verts[1] == verts[2] || verts[0] == verts[2])
        {
          return VTK_DEGENERATE_VERTEX;
        }
        this->T[++this->TMaxId] = t;
        for (j = 0; j < 3; j++)
        {
          if (verts[j] != sn.id && verts[j] != ptId)
          {
            nextVertex = verts[j];
            break;
          }
        }
        sn.id = nextVertex;
        this->Mesh->GetPoint(sn.id, sn.x);
        this->V[++this->VMaxId] = sn;
        this->EdgeNeighbors.Get(t.id, ptId, nextVertex, this->Neighbors);
        numNei = this->Neighbors->GetNumberOfIds();
      }

      if (this->TCount() == numTris)
      {
        // Reverse the loop to keep orientation consistent (L689-709).
        numVerts = this->VCount();
        for (i = 0; i < (numVerts / 2); i++)
        {
          LocalVertex tmp = this->V[i];
          this->V[i] = this->V[numVerts - i - 1];
          this->V[numVerts - i - 1] = tmp;
        }
        vtkIdType nt = this->TCount();
        for (i = 0; i < (nt / 2); i++)
        {
          vtkIdType tid = this->T[i].id;
          this->T[i].id = this->T[nt - i - 1].id;
          this->T[nt - i - 1].id = tid;
        }
        this->V[0].FAngle = -1.0;
        this->V[this->VMaxId].FAngle = -1.0;
        vtype = VTK_BOUNDARY_VERTEX;
      }
      else
      {
        vtype = VTK_NON_MANIFOLD_VERTEX;
      }
    }

    // Geometry: average plane (L726-788).
    x2 = this->V[0].x;
    for (i = 0; i < 3; i++)
    {
      v2[i] = x2[i] - this->X[i];
    }
    this->LoopArea = 0.0;
    this->Normal[0] = this->Normal[1] = this->Normal[2] = 0.0;
    this->Pt[0] = this->Pt[1] = this->Pt[2] = 0.0;
    numNormals = 0;

    for (i = 0; i < this->TCount(); i++)
    {
      normal = this->T[i].n;
      x1 = x2;
      x2 = this->V[i + 1].x;
      for (j = 0; j < 3; j++)
      {
        v1[j] = v2[j];
        v2[j] = x2[j] - this->X[j];
      }
      this->T[i].area = vtkTriangle::TriangleArea(this->X, x1, x2);
      vtkTriangle::TriangleCenter(this->X, x1, x2, center);
      this->LoopArea += this->T[i].area;
      vtkMath::Cross(v1, v2, normal);
      if (vtkMath::Normalize(normal) != 0.0)
      {
        numNormals++;
        for (j = 0; j < 3; j++)
        {
          this->Normal[j] += this->T[i].area * normal[j];
          this->Pt[j] += this->T[i].area * center[j];
        }
      }
    }
    if (!numNormals || this->LoopArea == 0.0)
    {
      return VTK_DEGENERATE_VERTEX;
    }
    for (j = 0; j < 3; j++)
    {
      this->Normal[j] /= this->LoopArea;
      this->Pt[j] /= this->LoopArea;
    }
    if (vtkMath::Normalize(this->Normal) == 0.0)
    {
      return VTK_DEGENERATE_VERTEX;
    }

    // Feature edges (L795-829).
    if (vtype == VTK_BOUNDARY_VERTEX)
    {
      numFEdges = 2;
      fedges[0] = 0;
      fedges[1] = this->VMaxId;
    }
    else
    {
      numFEdges = 0;
    }
    if (vtype == VTK_SIMPLE_VERTEX)
    {
      this->V[0].FAngle = vtkMath::Dot(this->T[0].n, this->T[this->TMaxId].n);
      if (this->V[0].FAngle <= this->CosAngle)
      {
        fedges[numFEdges++] = 0;
      }
    }
    for (i = 0; i < this->TMaxId; i++)
    {
      this->V[i + 1].FAngle = vtkMath::Dot(this->T[i].n, this->T[i + 1].n);
      if (this->V[i + 1].FAngle <= this->CosAngle)
      {
        if (numFEdges >= 2)
        {
          numFEdges++;
        }
        else
        {
          fedges[numFEdges++] = i + 1;
        }
      }
    }

    // Final classification (L833-863).
    if (vtype == VTK_SIMPLE_VERTEX && numFEdges > 0)
    {
      if (numFEdges == 1)
      {
        vtype = VTK_EDGE_END_VERTEX;
      }
      else if (numFEdges == 2)
      {
        vtype = VTK_INTERIOR_EDGE_VERTEX;
      }
      else
      {
        vtype = VTK_CORNER_VERTEX;
      }
    }
    else if (vtype == VTK_BOUNDARY_VERTEX)
    {
      if (numFEdges != 2)
      {
        vtype = VTK_CORNER_VERTEX;
      }
      else
      {
        if (this->V[fedges[0]].x[0] == this->V[fedges[1]].x[0] &&
          this->V[fedges[0]].x[1] == this->V[fedges[1]].x[1] &&
          this->V[fedges[0]].x[2] == this->V[fedges[1]].x[2])
        {
          vtype = VTK_CRACK_TIP_VERTEX;
        }
      }
    }
    return vtype;
  }

  // ----- Port of vtkDecimatePro::SplitLoop (L1362-1383).
  void SplitLoop(vtkIdType fedges[2], vtkIdType& n1, vtkIdType* l1, vtkIdType& n2, vtkIdType* l2)
  {
    vtkIdType i;
    vtkIdType* loop;
    vtkIdType* count;
    n1 = n2 = 0;
    loop = l1;
    count = &n1;
    for (i = 0; i <= this->VMaxId; i++)
    {
      loop[(*count)++] = i;
      if (i == fedges[0] || i == fedges[1])
      {
        loop = (loop == l1 ? l2 : l1);
        count = (count == &n1 ? &n2 : &n1);
        loop[(*count)++] = i;
      }
    }
  }

  // ----- Port of vtkDecimatePro::IsValidSplit (L1281-1357).
  int IsValidSplit(int index)
  {
    vtkIdType fedges[2];
    int i, sign;
    vtkIdType nverts = this->VMaxId + 1, j;
    double *x, val, sPt[3], v21[3], sN[3];
    vtkIdType l1[FVTK_DECIM_MAX_TRIS_PER_VERTEX], l2[FVTK_DECIM_MAX_TRIS_PER_VERTEX];
    vtkIdType n1, n2;

    fedges[0] = index;
    for (j = 0; j < (nverts - 3); j++)
    {
      fedges[1] = (index + 2 + j) % nverts;
      this->SplitLoop(fedges, n1, l1, n2, l2);

      for (i = 0; i < 3; i++)
      {
        sPt[i] = this->V[fedges[0]].x[i];
        v21[i] = this->V[fedges[1]].x[i] - sPt[i];
      }
      vtkMath::Cross(v21, this->Normal, sN);
      if (vtkMath::Normalize(sN) == 0.0)
      {
        return 0;
      }

      for (sign = 0, i = 0; i < n1; i++)
      {
        if (!(l1[i] == fedges[0] || l1[i] == fedges[1]))
        {
          x = this->V[l1[i]].x;
          val = vtkPlane::Evaluate(sN, sPt, x);
          if (std::fabs(val) < this->Tolerance)
          {
            return 0;
          }
          if (!sign)
          {
            sign = (val > this->Tolerance ? 1 : -1);
          }
          else if (sign != (val > 0 ? 1 : -1))
          {
            return 0;
          }
        }
      }
      sign *= -1;
      for (i = 0; i < n2; i++)
      {
        if (!(l2[i] == fedges[0] || l2[i] == fedges[1]))
        {
          x = this->V[l2[i]].x;
          val = vtkPlane::Evaluate(sN, sPt, x);
          if (std::fabs(val) < this->Tolerance)
          {
            return 0;
          }
          if (!sign)
          {
            sign = (val > this->Tolerance ? 1 : -1);
          }
          else if (sign != (val > 0 ? 1 : -1))
          {
            return 0;
          }
        }
      }
    }
    return 1;
  }

  // ----- Port of the SIMPLE and BOUNDARY cases of vtkDecimatePro::FindSplit
  // (L1125-1232). Returns collapseId (the existing input vertex to collapse
  // onto) or -1 if no valid collapse exists. Fills out.pt1/pt2/tri0/tri1/
  // numDeleted. Does NOT use a priority queue; for SIMPLE we replicate the
  // "shortest edge first that IsValidSplit" selection by scanning all loop
  // vertices in increasing edge length (stable on ties by index, matching the
  // priority-queue order closely enough for the geometry gate).
  vtkIdType FindSplit(int type, vtkIdType fedges[2], VertexEval& out)
  {
    vtkIdType numVerts = this->VMaxId + 1;

    if (type == VTK_SIMPLE_VERTEX)
    {
      // Order loop vertices by squared edge length to X (mirrors EdgeLengths
      // priority queue, L1149-1165), pick the first whose split is valid.
      std::vector<std::pair<double, vtkIdType>> order;
      order.reserve(static_cast<size_t>(numVerts));
      for (vtkIdType i = 0; i < numVerts; i++)
      {
        double d2 = vtkMath::Distance2BetweenPoints(this->X, this->V[i].x);
        order.emplace_back(d2, i);
      }
      std::sort(order.begin(), order.end());

      vtkIdType maxI = -1;
      for (const auto& pr : order)
      {
        if (this->IsValidSplit(static_cast<int>(pr.second)))
        {
          maxI = pr.second;
          break;
        }
      }
      if (maxI < 0)
      {
        return -1;
      }
      out.numDeleted = 2;
      out.tri0 = this->T[maxI].id;
      if (maxI == 0)
      {
        out.pt1 = this->V[1].id;
        out.pt2 = this->V[this->VMaxId].id;
        out.tri1 = this->T[this->TMaxId].id;
      }
      else
      {
        out.pt1 = this->V[(maxI + 1) % numVerts].id;
        out.pt2 = this->V[maxI - 1].id;
        out.tri1 = this->T[maxI - 1].id;
      }
      return this->V[maxI].id;
    }
    else if (type == VTK_BOUNDARY_VERTEX)
    {
      double dist2 = vtkMath::Distance2BetweenPoints(this->X, this->V[0].x);
      double e2dist2 = vtkMath::Distance2BetweenPoints(this->X, this->V[this->VMaxId].x);
      vtkIdType maxI = -1;
      if (dist2 <= e2dist2)
      {
        if (this->IsValidSplit(0))
        {
          maxI = 0;
        }
        else if (this->IsValidSplit(this->VMaxId))
        {
          maxI = this->VMaxId;
        }
      }
      else
      {
        if (this->IsValidSplit(this->VMaxId))
        {
          maxI = this->VMaxId;
        }
        else if (this->IsValidSplit(0))
        {
          maxI = 0;
        }
      }
      if (maxI < 0)
      {
        return -1;
      }
      out.numDeleted = 1;
      if (maxI == 0)
      {
        out.tri0 = this->T[0].id;
        out.pt1 = this->V[1].id;
        return this->V[0].id;
      }
      else
      {
        out.tri0 = this->T[this->TMaxId].id;
        out.pt1 = this->V[this->VMaxId - 1].id;
        return this->V[this->VMaxId].id;
      }
    }
    (void)fedges;
    return -1;
  }

  // Full evaluation of a single live vertex; fills `ev`. Read-only on the mesh.
  void Evaluate(vtkIdType ptId, VertexEval& ev)
  {
    ev = VertexEval{};

    vtkIdType ncells;
    vtkIdType* cells;
    this->Mesh->GetPointCells(ptId, ncells, cells);
    if (ncells <= 0)
    {
      return;
    }
    this->Mesh->GetPoint(ptId, this->X);

    vtkIdType fedges[2];
    int type = this->EvaluateVertex(ptId, ncells, cells, fedges);
    ev.vtype = type;

    // Only SIMPLE and (optionally) BOUNDARY are removable in v1. Everything
    // else (interior-edge / corner / crack / edge-end / non-manifold /
    // degenerate / high-degree) is conservatively NOT removable -> the host
    // serial path handles them when we fall back.
    bool eligible = (type == VTK_SIMPLE_VERTEX) ||
      (type == VTK_BOUNDARY_VERTEX && this->BoundaryDeletion);
    if (!eligible)
    {
      return;
    }

    // Compute the simple-type error exactly as vtkDecimatePro::Insert (L1625-
    // 1644): plane error for SIMPLE; edge / single-triangle error for BOUNDARY.
    double error;
    if (type == VTK_SIMPLE_VERTEX)
    {
      error = ComputeSimpleError(this->X, this->Normal, this->Pt);
    }
    else // BOUNDARY
    {
      if (ncells == 1)
      {
        error = ComputeSingleTriangleError(this->X, this->V[0].x, this->V[1].x);
      }
      else
      {
        error = ComputeEdgeError(this->X, this->V[fedges[0]].x, this->V[fedges[1]].x);
      }
    }
    ev.error = error;
    if (error > this->Error)
    {
      return; // exceeds MaximumError cap -> not removable
    }

    vtkIdType collapseId = this->FindSplit(type, fedges, ev);
    if (collapseId < 0)
    {
      return;
    }
    ev.collapseId = collapseId;
    ev.removable = true;
  }
};

// ---------------------------------------------------------------------------
// Apply one half-edge collapse in place. A faithful port of the numDeleted==2
// (SIMPLE) and numDeleted==1 (BOUNDARY) branches of vtkDecimatePro::CollapseEdge
// (L1388-1455), using the SAME vtkPolyData link-edit primitives. The DecimateLocal
// that produced `ev` is passed so its V/T loop (recomputed) drives the rewiring.
//
// IMPORTANT: this re-runs EvaluateVertex on the (still-pristine) neighborhood to
// regather the ordered V/T loop for ptId; under distance-2 MIS no other thread
// has touched this neighborhood, so the regather sees the same topology the
// selection saw. Returns the number of triangles eliminated (1 or 2).
int ApplyCollapse(DecimateLocal& dl, vtkIdType ptId, const VertexEval& sel)
{
  vtkPolyData* mesh = dl.Mesh;

  // Regather the loop for ptId (read-only; matches selection-time state).
  vtkIdType ncells;
  vtkIdType* cells;
  mesh->GetPointCells(ptId, ncells, cells);
  if (ncells <= 0)
  {
    return 0;
  }
  mesh->GetPoint(ptId, dl.X);
  vtkIdType fedges[2];
  VertexEval ev;
  ev = VertexEval{};
  int type = dl.EvaluateVertex(ptId, ncells, cells, fedges);
  if (type != sel.vtype)
  {
    return 0; // neighborhood changed unexpectedly -> skip (defensive)
  }
  vtkIdType collapseId = dl.FindSplit(type, fedges, ev);
  if (collapseId != sel.collapseId || ev.numDeleted != sel.numDeleted)
  {
    return 0; // non-deterministic mismatch -> skip (defensive)
  }

  vtkIdType ntris = dl.TMaxId + 1;
  vtkIdType nverts = dl.VMaxId + 1;
  vtkIdType pt1 = ev.pt1, pt2 = ev.pt2;
  vtkIdType tri0 = ev.tri0, tri1 = ev.tri1;
  int numDeleted = ev.numDeleted;

  if (numDeleted == 2) // SIMPLE (CollapseEdge L1404-1432, non-crack branch).
  {
    mesh->RemoveReferenceToCell(pt1, tri0);
    mesh->RemoveReferenceToCell(pt2, tri1);
    mesh->RemoveReferenceToCell(collapseId, tri0);
    mesh->RemoveReferenceToCell(collapseId, tri1);
    mesh->DeletePoint(ptId);
    mesh->DeleteCell(tri0);
    mesh->DeleteCell(tri1);

    mesh->ResizeCellList(collapseId, ntris - 2);
    for (vtkIdType i = 0; i < ntris; i++)
    {
      if (dl.T[i].id != tri0 && dl.T[i].id != tri1)
      {
        mesh->AddReferenceToCell(collapseId, dl.T[i].id);
        mesh->ReplaceCellPoint(dl.T[i].id, ptId, collapseId);
      }
    }
  }
  else if (numDeleted == 1) // BOUNDARY (CollapseEdge L1434-1455).
  {
    mesh->RemoveReferenceToCell(pt1, tri0);
    mesh->RemoveReferenceToCell(collapseId, tri0);
    mesh->DeletePoint(ptId);
    mesh->DeleteCell(tri0);
    if (ntris > 1)
    {
      mesh->ResizeCellList(collapseId, ntris - 1);
      for (vtkIdType i = 0; i < ntris; i++)
      {
        if (dl.T[i].id != tri0)
        {
          mesh->AddReferenceToCell(collapseId, dl.T[i].id);
          mesh->ReplaceCellPoint(dl.T[i].id, ptId, collapseId);
        }
      }
    }
  }
  else
  {
    return 0;
  }
  (void)nverts;
  return numDeleted;
}

VTK_ABI_NAMESPACE_END
} // anonymous namespace

namespace fvtk
{
VTK_ABI_NAMESPACE_BEGIN

std::uint64_t GetFastDecimateEngageCount()
{
  return g_engageCount.load(std::memory_order_relaxed);
}

bool FastDecimatePro(vtkPolyData* input, vtkPolyData* output, double targetReduction,
  double featureAngle, bool boundaryVertexDeletion, bool preserveTopology, double maximumError,
  int outputPointsPrecision)
{
  if (!FastModeEnabled())
  {
    return false;
  }

  // --- Regime gate ---------------------------------------------------------
  if (preserveTopology) // the kernel never splits; topology preservation needs serial.
  {
    return false;
  }
  if (!(targetReduction > 0.0 && targetReduction <= 1.0))
  {
    return false;
  }
  if (!input)
  {
    return false;
  }
  // No verts / lines / strips: DecimatePro is polys-only and the kernel assumes
  // triangle links only.
  if (input->GetNumberOfVerts() != 0 || input->GetNumberOfLines() != 0 ||
    input->GetNumberOfStrips() != 0)
  {
    return false;
  }
  vtkCellArray* inPolys = input->GetPolys();
  vtkPoints* inPts = input->GetPoints();
  if (!inPolys || !inPts || !inPts->GetData())
  {
    return false;
  }
  const vtkIdType numTris = input->GetNumberOfPolys();
  const vtkIdType numPts = input->GetNumberOfPoints();
  if (numTris < 1 || numPts < 1 || numPts > static_cast<vtkIdType>(0x7FFFFFFF) ||
    numTris > static_cast<vtkIdType>(0x7FFFFFFF))
  {
    return false;
  }
  // All-triangle requirement (matches vtkDecimatePro.cxx L177-187).
  if (inPolys->IsHomogeneous() != 3)
  {
    return false;
  }
  const int ptType = inPts->GetData()->GetDataType();
  if (ptType != VTK_FLOAT && ptType != VTK_DOUBLE)
  {
    return false;
  }

  const vtkIdType targetEliminated = static_cast<vtkIdType>(std::lround(targetReduction * numTris));
  if (targetEliminated <= 0)
  {
    return false; // nothing to do via fast path; let serial handle the trivial case
  }

  // --- Build a private editable working mesh (serial O(n); mirrors L191-234).
  vtkNew<vtkPolyData> mesh;
  vtkNew<vtkPoints> meshPts;
  // Preserve precision per outputPointsPrecision. DEFAULT -> input type.
  if (outputPointsPrecision == vtkAlgorithm::SINGLE_PRECISION)
  {
    meshPts->SetDataType(VTK_FLOAT);
  }
  else if (outputPointsPrecision == vtkAlgorithm::DOUBLE_PRECISION)
  {
    meshPts->SetDataType(VTK_DOUBLE);
  }
  else
  {
    meshPts->SetDataType(inPts->GetDataType());
  }
  meshPts->SetNumberOfPoints(numPts);
  meshPts->DeepCopy(inPts);
  mesh->SetPoints(meshPts);

  vtkNew<vtkCellArray> meshPolys;
  meshPolys->DeepCopy(inPolys);
  // THREAD-SAFETY: the parallel EVALUATE/SELECT steps call the direct-pointer
  // GetCellPoints(cellId, npts, pts) on the working mesh from many threads. With
  // fvtk's default int32 cell storage that 2-arg accessor copies into a SHARED
  // per-vtkCellArray scratch buffer (it cannot hand back an int32* as a
  // vtkIdType*), so concurrent calls clobber each other (the #114 class of bug).
  // Forcing 64-bit storage makes GetCellPoints return a direct pointer into the
  // connectivity array -> no shared scratch -> reentrant. (The values are
  // identical; only the container width changes, which the order-relaxed output
  // gate permits.)
  if (!meshPolys->IsStorage64Bit())
  {
    meshPolys->ConvertTo64BitStorage(); // value-preserving width change
  }
  mesh->SetPolys(meshPolys);

  vtkPointData* meshPD = mesh->GetPointData();
  meshPD->DeepCopy(input->GetPointData());

  mesh->EditableOn();
  mesh->BuildLinks();
  // Force the lazy Cells (TaggedCellId) build now, on this single thread, so the
  // parallel EVALUATE/SELECT loops never trigger it concurrently (the inline
  // GetCellPoints/GetCellType paths assume Cells is already built; fvtk also
  // guards the lazy build with a mutex, but building it eagerly avoids that
  // contention and any first-touch ambiguity).
  mesh->BuildCells();

  const double cosAngle = std::cos(vtkMath::RadiansFromDegrees(featureAngle));
  const double tolerance = FVTK_DECIM_TOLERANCE * input->GetLength();
  // MaximumError cap (L155-168): a fraction of the bbox max extent (ErrorIsAbsolute
  // is always 0 on the fast path; AbsoluteError handling stays serial).
  const double* bnds = input->GetBounds();
  double maxExtent = 0.0;
  for (int i = 0; i < 3; i++)
  {
    double e = bnds[2 * i + 1] - bnds[2 * i];
    maxExtent = (e > maxExtent ? e : maxExtent);
  }
  const double errCap =
    (maximumError >= VTK_DOUBLE_MAX ? VTK_DOUBLE_MAX : maximumError * maxExtent);

  // Per-vertex working arrays.
  std::vector<unsigned char> removed(static_cast<size_t>(numPts), 0);
  std::vector<vtkIdType> collapseTarget(static_cast<size_t>(numPts), -1);
  std::vector<double> error(static_cast<size_t>(numPts), VTK_DOUBLE_MAX);
  std::vector<unsigned char> selected(static_cast<size_t>(numPts), 0);
  std::vector<VertexEval> evals(static_cast<size_t>(numPts));

  // Thread-local reentrant evaluators.
  auto makeLocal = [&]() { return DecimateLocal(mesh, cosAngle, tolerance, errCap, boundaryVertexDeletion); };

  vtkIdType eliminated = 0;
  bool progress = true;

  // Distance-1 graph neighbors of a live vertex via its incident triangles
  // (read-only on links). Used for the distance-2 MIS test.
  auto forEachNeighbor = [&](vtkPolyData* m, vtkIdType v, auto&& fn) {
    vtkIdType nc;
    vtkIdType* cs;
    m->GetPointCells(v, nc, cs);
    for (vtkIdType c = 0; c < nc; c++)
    {
      vtkIdType np;
      const vtkIdType* pp;
      m->GetCellPoints(cs[c], np, pp);
      for (vtkIdType k = 0; k < np; k++)
      {
        if (pp[k] != v)
        {
          fn(pp[k]);
        }
      }
    }
  };

  while (eliminated < targetEliminated && progress)
  {
    progress = false;

    // ----- WAVE STEP 1: PARALLEL EVALUATE (read-only on mesh) --------------
    {
      vtkSMPThreadLocal<DecimateLocal> tlsLocal(makeLocal());
      RunFastFilterParallel([&]() {
        vtkSMPTools::For(0, numPts, [&](vtkIdType begin, vtkIdType end) {
          DecimateLocal& dl = tlsLocal.Local();
          for (vtkIdType v = begin; v < end; v++)
          {
            if (removed[v])
            {
              evals[v].removable = false;
              error[v] = VTK_DOUBLE_MAX;
              collapseTarget[v] = -1;
              continue;
            }
            VertexEval ev;
            dl.Evaluate(v, ev);
            evals[v] = ev;
            error[v] = ev.removable ? ev.error : VTK_DOUBLE_MAX;
            collapseTarget[v] = ev.removable ? ev.collapseId : -1;
          }
        });
      });
    }

    // ----- WAVE STEP 2: SELECT DISTANCE-2 MIS (deterministic) --------------
    // Select v iff v is removable AND a STRICT local minimum in (error, id)
    // among all vertices within graph distance 2 (and every such neighbor is
    // either removed or evaluated). Distance-2 because a collapse touches the
    // union of the loops of v AND collapseTarget[v]; two selected verts must
    // have disjoint 2-neighborhoods. Read-only on shared state -> deterministic.
    std::fill(selected.begin(), selected.end(), 0);
    {
      // No per-thread evaluator needed: this step only reads error[]/removed[]/
      // evals[].removable plus the (read-only) graph neighborhood.
      RunFastFilterParallel([&]() {
        vtkSMPTools::For(0, numPts, [&](vtkIdType begin, vtkIdType end) {
          for (vtkIdType v = begin; v < end; v++)
          {
            if (removed[v] || !evals[v].removable)
            {
              continue;
            }
            const double ev = error[v];
            bool isMin = true;
            // Distance-2 closed neighborhood strict-min test.
            forEachNeighbor(mesh, v, [&](vtkIdType u) {
              if (!isMin || u == v)
              {
                return;
              }
              if (!removed[u])
              {
                // distance-1 competitor
                if (evals[u].removable)
                {
                  if (error[u] < ev || (error[u] == ev && u < v))
                  {
                    isMin = false;
                    return;
                  }
                }
              }
              forEachNeighbor(mesh, u, [&](vtkIdType w) {
                if (!isMin || w == v || w == u)
                {
                  return;
                }
                if (!removed[w] && evals[w].removable)
                {
                  if (error[w] < ev || (error[w] == ev && w < v))
                  {
                    isMin = false;
                  }
                }
              });
            });
            if (isMin)
            {
              selected[v] = 1;
            }
          }
        });
      });
    }

    // ----- FINAL-WAVE PREFIX: cap selection so we don't overshoot ----------
    // Gather selected ids; if applying all would overshoot the target, apply
    // only the lowest-(error,id) prefix that reaches it. Serial gather (cheap).
    std::vector<vtkIdType> sel;
    sel.reserve(64);
    for (vtkIdType v = 0; v < numPts; v++)
    {
      if (selected[v])
      {
        sel.push_back(v);
      }
    }
    if (sel.empty())
    {
      break; // no progress
    }
    // Each selected collapse removes evals[v].numDeleted tris. Sort by (error,id)
    // and keep a prefix whose cumulative removal first reaches the remaining gap.
    std::sort(sel.begin(), sel.end(), [&](vtkIdType a, vtkIdType b) {
      if (error[a] != error[b])
      {
        return error[a] < error[b];
      }
      return a < b;
    });
    const vtkIdType gap = targetEliminated - eliminated;
    {
      vtkIdType cum = 0;
      size_t keep = sel.size();
      for (size_t i = 0; i < sel.size(); i++)
      {
        cum += evals[sel[i]].numDeleted;
        if (cum >= gap)
        {
          keep = i + 1;
          break;
        }
      }
      if (keep < sel.size())
      {
        sel.resize(keep);
      }
    }

    // ----- WAVE STEP 3: PARALLEL APPLY (disjoint 2-neighborhoods) ----------
    vtkSMPThreadLocal<vtkIdType> tlsElim(0);
    {
      vtkSMPThreadLocal<DecimateLocal> tlsApply(makeLocal());
      const vtkIdType nsel = static_cast<vtkIdType>(sel.size());
      RunFastFilterParallel([&]() {
        vtkSMPTools::For(0, nsel, [&](vtkIdType begin, vtkIdType end) {
          DecimateLocal& dl = tlsApply.Local();
          vtkIdType& localElim = tlsElim.Local();
          for (vtkIdType i = begin; i < end; i++)
          {
            vtkIdType v = sel[i];
            int d = ApplyCollapse(dl, v, evals[v]);
            if (d > 0)
            {
              removed[v] = 1;
              localElim += d;
            }
          }
        });
      });
    }
    vtkIdType waveElim = 0;
    for (auto it = tlsElim.begin(); it != tlsElim.end(); ++it)
    {
      waveElim += *it;
    }
    if (waveElim > 0)
    {
      eliminated += waveElim;
      progress = true;
    }
  }

  // --- FALLBACK CONTRACT: must reach the target within tolerance ----------
  // Tolerance: within max(2%, one triangle) of the requested reduction.
  const double achieved = static_cast<double>(eliminated) / static_cast<double>(numTris);
  const double tol = std::max(0.02, 1.0 / static_cast<double>(numTris));
  if (achieved < targetReduction - tol)
  {
    return false; // DISCARD partial result; host runs serial.
  }

  // --- Output assembly (serial O(n); mirrors L370-419) --------------------
  vtkPoints* mPts = mesh->GetPoints();
  vtkPointData* outPD = output->GetPointData();

  std::vector<vtkIdType> map(static_cast<size_t>(numPts), -1);
  vtkIdType numNewPts = 0;
  for (vtkIdType ptId = 0; ptId < numPts; ptId++)
  {
    vtkIdType nc;
    vtkIdType* cs;
    mesh->GetPointCells(ptId, nc, cs);
    if (nc > 0)
    {
      map[ptId] = numNewPts++;
    }
  }

  vtkNew<vtkPoints> newPts;
  newPts->SetDataType(mPts->GetDataType());
  newPts->SetNumberOfPoints(numNewPts);
  outPD->CopyAllocate(meshPD, numNewPts);
  for (vtkIdType ptId = 0; ptId < numPts; ptId++)
  {
    if (map[ptId] >= 0)
    {
      double p[3];
      mPts->GetPoint(ptId, p);
      newPts->SetPoint(map[ptId], p); // exact copy of an existing input coordinate
      outPD->CopyData(meshPD, ptId, map[ptId]);
    }
  }

  vtkNew<vtkCellArray> newPolys;
  newPolys->AllocateEstimate(numTris - eliminated, 3);
  vtkCellData* inCD = input->GetCellData();
  vtkCellData* outCD = output->GetCellData();
  outCD->CopyAllocate(inCD, numTris - eliminated);
  vtkIdType newCellId = 0;
  for (vtkIdType cellId = 0; cellId < numTris; cellId++)
  {
    if (mesh->GetCellType(cellId) == VTK_TRIANGLE) // non-deleted
    {
      vtkIdType np;
      const vtkIdType* pp;
      mesh->GetCellPoints(cellId, np, pp);
      vtkIdType ncp[3] = { map[pp[0]], map[pp[1]], map[pp[2]] };
      newPolys->InsertNextCell(3, ncp);
      outCD->CopyData(inCD, cellId, newCellId++);
    }
  }

  output->SetPoints(newPts);
  output->SetPolys(newPolys);

  g_engageCount.fetch_add(1, std::memory_order_relaxed);
  return true;
}

VTK_ABI_NAMESPACE_END
} // namespace fvtk
