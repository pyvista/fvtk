// SPDX-License-Identifier: BSD-3-Clause
// cvista opt-in fast polygon-orientation pass: VTK adapter over the cvista parallel
// orientation kernel (pvaOrient.h). This TU is built with OpenMP and excluded
// from the unity build so the kernel + <omp.h> stay isolated, mirroring
// cvistaFastClean / cvistaFastConnectivity.
#include "cvistaFastOrient.h"

#include "vtkCVISTASMPDefaults.h" // cvista::FastModeEnabled (needed even without OpenMP)

#ifdef CVISTA_HAVE_OPENMP

#include "vtkPolyData.h"

#include "pvaOrient.h" // pva::orient::Run (pulls in <omp.h>)

#include <cstdio>  // std::fopen / std::fclose (engagement sentinel)
#include <cstdlib> // std::getenv
#include <vector>

namespace
{
// Engagement sentinel: when the env var CVISTA_FAST_ORIENT_SENTINEL names a path,
// touch that file the first time the parallel orientation path actually executes
// in this process. The relaxed parity gate also passes on the stock fallback, so
// the bitexact op uses this sentinel to PROVE the fast kernel really ran under
// EnableFast() (see tests/bitexact/ops.py::op_orient_fast). No-op when the env
// var is unset (production / non-test runs).
void TouchEngagementSentinel()
{
  const char* path = std::getenv("CVISTA_FAST_ORIENT_SENTINEL");
  if (path == nullptr || path[0] == '\0')
  {
    return;
  }
  if (FILE* f = std::fopen(path, "wb"))
  {
    std::fputc('1', f);
    std::fclose(f);
  }
}
}

#endif // CVISTA_HAVE_OPENMP

namespace cvista
{
VTK_ABI_NAMESPACE_BEGIN

bool FastOrientPolyData(vtkPolyData* input, vtkPolyData* output, bool consistency,
  bool flipNormals, bool autoOrient, bool nonManifoldTraversal)
{
  if (!FastModeEnabled())
  {
    return false;
  }
#ifndef CVISTA_HAVE_OPENMP
  (void)input;
  (void)output;
  (void)consistency;
  (void)flipNormals;
  (void)autoOrient;
  (void)nonManifoldTraversal;
  return false;
#else
  // Supported regime only: manifold, consistency-only orientation. The
  // AutoOrientNormals "leftmost cell" seeding and NonManifoldTraversal both keep
  // the byte-exact serial path (the kernel models neither).
  if (!consistency || autoOrient || nonManifoldTraversal)
  {
    return false;
  }
  if (input == nullptr || output == nullptr)
  {
    return false;
  }
  // The kernel queries edge neighbors off the input's (already-built) cell links;
  // bail to the serial path if links/cells are not present.
  if (input->GetLinks() == nullptr)
  {
    return false;
  }
  const vtkIdType numCells = input->GetNumberOfCells();
  if (numCells != output->GetNumberOfCells())
  {
    return false; // structural mismatch -> defer to stock
  }

  // Run the parallel orientation kernel against the input's connectivity/links.
  pva::orient::OrientResult r = pva::orient::Run(input, flipNormals);
  if (!r.ok)
  {
    // Non-manifold (an edge shared by > 2 cells) -> fall back to the serial path.
    return false;
  }

  TouchEngagementSentinel();

  // Apply the flips to the OUTPUT mesh (a DeepCopy of the input polys with the
  // same point-id connectivity). ReverseCell touches only this cell's own
  // contiguous connectivity slice, so the per-cell reversals are independent and
  // safe to run in parallel. ReverseCell on vtkPolyData is pure index bookkeeping
  // over the cell array (no shared mutable state across distinct cell ids).
  if (numCells > 0)
  {
#pragma omp parallel for schedule(static)
    for (vtkIdType c = 0; c < numCells; ++c)
    {
      if (r.flip[c])
      {
        output->ReverseCell(c);
      }
    }
  }

  return true;
#endif // CVISTA_HAVE_OPENMP
}

VTK_ABI_NAMESPACE_END
} // namespace cvista
