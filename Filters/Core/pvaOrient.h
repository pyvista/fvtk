// SPDX-License-Identifier: BSD-3-Clause
//
// cvista opt-in fast polygon-orientation kernel (an cvista original, not vendored).
//
// Parallel, deterministic replacement for vtkOrientPolyData::TraverseAndOrder --
// the serial single-threaded BFS wave that propagates a consistent winding
// across each connected component of a manifold surface. It is the last fully
// serial, order-locked stage in the default vtkPolyDataNormals (Consistency=1)
// pipeline, so accelerating it speeds up .compute_normals() on large connected
// meshes with no change to vtkPolyDataNormals itself.
//
// ---------------------------------------------------------------------
// Design
// ---------------------------------------------------------------------
//
//   1. Parallel connected-component labels over the cell edge-neighbor graph,
//      via the same lock-free atomic union-find used by cvistaFastConnectivity
//      (link the larger root to the smaller, so each component root == its
//      MINIMUM cell index -- the canonical seed). The same pass detects
//      non-manifold edges (> 2 incident cells) and builds, per cell, the
//      manifold-edge adjacency annotated with the per-edge winding-consistency
//      bit `mustFlip`.
//   2. mustFlip(a,b) is exactly the inconsistent-winding test from
//      vtkOrientPolyData::TraverseAndOrder: across the shared edge, does neighbor
//      b wind the SAME direction as a (so b must be reversed to agree)? It is a
//      symmetric Z/2 relation on the orientable manifold.
//   3. Per-component absolute flip bit, resolved by a DETERMINISTIC BFS from each
//      component's canonical seed (lowest global cell id): abs[seed]=0 (XOR
//      flipAllNormals), abs[nbr] = abs[cur] XOR mustFlip(cur,nbr). The lowest-id
//      seed makes the per-component winding choice -- the ONLY thing relaxed vs
//      stock -- independent of thread count.
//   4. Parallel final pass: reverse every cell whose flip bit is set (done by the
//      caller via vtkPolyData::ReverseCell over disjoint per-cell connectivity).
//
// ---------------------------------------------------------------------
// Determinism / parity
// ---------------------------------------------------------------------
//
// vtkOrientPolyData never moves, adds, or deletes points or cells; it only
// ReverseCell()s (reverses the point-id order WITHIN a cell). The cell-to-slot
// mapping and every coordinate are untouched. The only relaxation vs stock is the
// per-component winding CHOICE, resolved deterministically by the lowest-cell-id
// seed. The order-relaxed parity gate asserts the real invariant: adjacent cells
// never disagree on winding.
//
// ---------------------------------------------------------------------
// Fallback regime (kernel returns ok=false -> caller runs the stock serial BFS)
// ---------------------------------------------------------------------
//
// Restricted to the MANIFOLD, consistency-only case. Any edge with > 2 incident
// cells routes to the byte-exact serial path; NonManifoldTraversal and
// AutoOrientNormals are screened out by the caller before this kernel is reached.
#ifndef pvaOrient_h
#define pvaOrient_h

#ifdef CVISTA_HAVE_OPENMP

#include "vtkIdList.h"
#include "vtkNew.h"
#include "vtkPolyData.h"
#include "vtkPolyDataEdgeNeighbors.h" // vtkPolyDataEdgeNeighbors::FastEdgeNeighbors

#include <cstdint>
#include <vector>

#include <omp.h>

namespace pva
{
namespace orient
{

// Concurrent disjoint-set, matching cvistaFastConnectivity's: relaxed atomics on
// parent[]; union links the LARGER root to the SMALLER so each component's final
// root is its MINIMUM cell index (the canonical seed).
inline vtkIdType ufLoad(vtkIdType* parent, vtkIdType x)
{
  return __atomic_load_n(&parent[x], __ATOMIC_RELAXED);
}

inline vtkIdType ufFind(vtkIdType* parent, vtkIdType x)
{
  vtkIdType p = ufLoad(parent, x);
  while (p != x)
  {
    const vtkIdType gp = ufLoad(parent, p);
    if (gp != p)
    {
      __atomic_store_n(&parent[x], gp, __ATOMIC_RELAXED);
    }
    x = p;
    p = gp;
  }
  return x;
}

inline void ufUnion(vtkIdType* parent, vtkIdType a, vtkIdType b)
{
  for (;;)
  {
    a = ufFind(parent, a);
    b = ufFind(parent, b);
    if (a == b)
    {
      return;
    }
    const vtkIdType hi = a > b ? a : b;
    const vtkIdType lo = a < b ? a : b;
    vtkIdType expected = hi;
    if (__atomic_compare_exchange_n(
          &parent[hi], &expected, lo, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
    {
      return;
    }
  }
}

// One annotated manifold-edge adjacency entry: a neighbor cell and the winding
// flip bit relating the two cells across their shared edge.
struct Edge
{
  vtkIdType nbr;
  unsigned char mustFlip;
};

// Result of the kernel: per-cell "reverse this cell?" bits (1 = reverse).
struct OrientResult
{
  std::vector<unsigned char> flip; // size == numCells
  bool ok = false;                 // false -> caller must run the stock path
};

// mustFlip(self, nbr): given self's winding, must nbr be reversed to be
// consistent across the shared edge (selfA -> selfB). This is the exact test
// from vtkOrientPolyData::TraverseAndOrder, with `self` in the role of the
// already-visited cell and `nbr` in the role of the candidate.
inline unsigned char EdgeMustFlip(
  vtkPolyData* mesh, vtkIdType selfA, vtkIdType selfB, vtkIdType nbr, vtkIdList* nbrPts)
{
  vtkIdType nnp;
  const vtkIdType* nbp;
  mesh->GetCellPoints(nbr, nnp, nbp, nbrPts);
  vtkIdType l = 0;
  for (; l < nnp; ++l)
  {
    if (nbp[l] == selfB)
    {
      break;
    }
  }
  // Consistent: nbr traverses the shared edge in the OPPOSITE direction, i.e.
  // after selfB comes selfA. If after selfB comes... not selfA, the neighbor
  // winds the same way and must be reversed. (Byte-identical to the stock
  // `neiPts[(l + 1) % numNeiPts] != pts[j]` test.)
  return (l < nnp && nbp[(l + 1) % nnp] != selfA) ? 1 : 0;
}

// Run the parallel orientation kernel on @p mesh (cells + links already built).
// @p flipAllNormals mirrors vtkOrientPolyData::FlipNormals: when true every
// component is inverted (stock pre-flips each component seed). Returns ok=false
// (flip empty) if the mesh is non-manifold (any edge shared by > 2 cells) -- the
// caller then falls back to the byte-exact serial BFS.
inline OrientResult Run(vtkPolyData* mesh, bool flipAllNormals)
{
  OrientResult res;
  const vtkIdType numCells = mesh->GetNumberOfCells();
  if (numCells < 1)
  {
    res.ok = true;
    return res;
  }

  const vtkPolyDataEdgeNeighbors::FastEdgeNeighbors edgeNeighbors(mesh);

  // ---- 1. union-find component labels + non-manifold screen --------------
  std::vector<vtkIdType> parent(static_cast<size_t>(numCells));
  // Per-cell adjacency (CSR built in two passes to avoid concurrent push_back).
  std::vector<vtkIdType> degree(static_cast<size_t>(numCells), 0);
  int nonManifold = 0;

#pragma omp parallel for schedule(static)
  for (vtkIdType i = 0; i < numCells; ++i)
  {
    parent[i] = i;
  }

  // Pass A: union neighbors, screen non-manifold, count per-cell manifold edges.
#pragma omp parallel
  {
    vtkNew<vtkIdList> cellPts;
    vtkNew<vtkIdList> nbrs;
#pragma omp for schedule(dynamic, 1024)
    for (vtkIdType cellId = 0; cellId < numCells; ++cellId)
    {
      if (__atomic_load_n(&nonManifold, __ATOMIC_RELAXED))
      {
        continue;
      }
      vtkIdType npts;
      const vtkIdType* pts;
      mesh->GetCellPoints(cellId, npts, pts, cellPts);
      if (npts < 3)
      {
        continue;
      }
      vtkIdType d = 0;
      for (vtkIdType j = 0, j1 = 1; j < npts; ++j, (j1 = (++j1 < npts) ? j1 : 0))
      {
        edgeNeighbors.Get(cellId, pts[j], pts[j1], nbrs);
        const vtkIdType nn = nbrs->GetNumberOfIds();
        if (nn > 1)
        {
          __atomic_store_n(&nonManifold, 1, __ATOMIC_RELAXED);
          break;
        }
        if (nn == 1)
        {
          ufUnion(parent.data(), cellId, nbrs->GetId(0));
          ++d;
        }
      }
      degree[cellId] = d;
    }
  }

  if (nonManifold)
  {
    res.ok = false;
    return res;
  }

  // CSR offsets from degrees (serial prefix sum; cheap, O(numCells)).
  std::vector<vtkIdType> rowStart(static_cast<size_t>(numCells) + 1, 0);
  for (vtkIdType i = 0; i < numCells; ++i)
  {
    rowStart[i + 1] = rowStart[i] + degree[i];
  }
  const vtkIdType numEdgeSlots = rowStart[numCells];
  std::vector<Edge> adj(static_cast<size_t>(numEdgeSlots));

  // Pass B: fill the adjacency with annotated neighbors (each cell writes only
  // its own contiguous CSR slice -> race-free).
#pragma omp parallel
  {
    vtkNew<vtkIdList> cellPts;
    vtkNew<vtkIdList> nbrPts;
    vtkNew<vtkIdList> nbrs;
#pragma omp for schedule(dynamic, 1024)
    for (vtkIdType cellId = 0; cellId < numCells; ++cellId)
    {
      vtkIdType npts;
      const vtkIdType* pts;
      mesh->GetCellPoints(cellId, npts, pts, cellPts);
      if (npts < 3)
      {
        continue;
      }
      vtkIdType w = rowStart[cellId];
      const vtkIdType wend = rowStart[cellId + 1];
      for (vtkIdType j = 0, j1 = 1; j < npts && w < wend; ++j, (j1 = (++j1 < npts) ? j1 : 0))
      {
        edgeNeighbors.Get(cellId, pts[j], pts[j1], nbrs);
        if (nbrs->GetNumberOfIds() != 1)
        {
          continue;
        }
        const vtkIdType nb = nbrs->GetId(0);
        const unsigned char mf = EdgeMustFlip(mesh, pts[j], pts[j1], nb, nbrPts);
        adj[w].nbr = nb;
        adj[w].mustFlip = mf;
        ++w;
      }
    }
  }

  // Flatten roots so parent[i] == component root (== min cell index).
#pragma omp parallel for schedule(static)
  for (vtkIdType i = 0; i < numCells; ++i)
  {
    parent[i] = ufFind(parent.data(), i);
  }

  // ---- 3. deterministic per-component BFS from the canonical seed ---------
  // Seeds are the union-find roots (parent[i] == i), visited in ascending id so
  // every component starts at its lowest cell id -> thread-count-invariant.
  res.flip.assign(static_cast<size_t>(numCells), 0);
  std::vector<unsigned char> visited(static_cast<size_t>(numCells), 0);
  std::vector<vtkIdType> stack;
  stack.reserve(256);
  const unsigned char inv = flipAllNormals ? 1 : 0;

  for (vtkIdType seed = 0; seed < numCells; ++seed)
  {
    if (parent[seed] != seed || visited[seed])
    {
      continue;
    }
    visited[seed] = 1;
    res.flip[seed] = inv; // canonical winding (optionally globally inverted)
    stack.push_back(seed);
    while (!stack.empty())
    {
      const vtkIdType cur = stack.back();
      stack.pop_back();
      const unsigned char curFlip = res.flip[cur];
      const vtkIdType e0 = rowStart[cur];
      const vtkIdType e1 = rowStart[cur + 1];
      for (vtkIdType e = e0; e < e1; ++e)
      {
        const vtkIdType nb = adj[e].nbr;
        if (visited[nb])
        {
          continue;
        }
        visited[nb] = 1;
        // If cur is (relative to canonical) flipped, the consistency it imposes
        // on nb inverts with it: nb's absolute flip = curFlip XOR mustFlip.
        res.flip[nb] = static_cast<unsigned char>(curFlip ^ adj[e].mustFlip);
        stack.push_back(nb);
      }
    }
  }

  res.ok = true;
  return res;
}

} // namespace orient
} // namespace pva

#endif // CVISTA_HAVE_OPENMP
#endif // pvaOrient_h
// VTK-HeaderTest-Exclude: pvaOrient.h
