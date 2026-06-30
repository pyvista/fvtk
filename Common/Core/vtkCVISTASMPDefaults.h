// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file   vtkCVISTASMPDefaults.h
 * @brief  cvista "multicore-by-default" policy for bit-exact-safe filters.
 *
 * This is an cvista (pyvista/cvista) addition, not part of upstream VTK.
 *
 * cvista ships with the global vtkSMPTools backend set to "Sequential", so by
 * default the WHOLE library runs serially and stays byte-for-byte identical to
 * stock VTK 9.6.2. A small, audited set of filters whose vtkSMPTools::For loops
 * are provably bit-exact under any thread count (each iteration writes only its
 * own pre-sized output slot, out[i] = f(in[i]); no append / no reduction /
 * no order-dependent insert) opt INTO multithreading locally via the helper in
 * this file, defaulting to a cap of 4 threads.
 *
 * The mechanism is vtkSMPTools::LocalScope: it temporarily activates the
 * STDThread backend (compiled in) for the duration of the filter's parallel
 * region, then restores the previous global state -- so nothing else in the
 * library is affected and the global default stays Sequential.
 *
 * User overrides through the EXISTING VTK SMP APIs are honored (see the
 * precedence table in vtkCVISTASMPDefaults.cxx):
 *   - VTK_SMP_MAX_THREADS env var  : raise / lower the count (=1 -> serial).
 *   - vtkSMPTools::Initialize(n)    : explicit programmatic thread count.
 *   - vtkSMPTools::SetBackend(...)  : pick a backend for the whole library, or
 *     set VTK_SMP_BACKEND_IN_USE=STDThread to thread EVERYTHING (not bit-exact).
 *   - CVISTA_SMP_DEFAULT=0 (cvista env) : turn this default OFF -> everything serial.
 */

#ifndef vtkCVISTASMPDefaults_h
#define vtkCVISTASMPDefaults_h

#include "vtkCommonCoreModule.h" // For export macro
#include "vtkSMPTools.h"         // For vtkSMPTools::Config / LocalScope / IsParallelScope

#include <utility> // For std::forward

namespace cvista
{
VTK_ABI_NAMESPACE_BEGIN

/**
 * Default cap on the number of threads used by bit-exact-safe filters that opt
 * into multithreading out of the box. User overrides (env / Initialize /
 * SetBackend) take precedence; see GetSafeFilterThreadingConfig().
 */
constexpr int CVISTA_SMP_DEFAULT_THREADS = 4;

/**
 * Compute the vtkSMPTools::Config that an audited bit-exact-safe filter should
 * apply around its parallel region. Encodes the override precedence; the
 * returned Config either selects STDThread (with a thread count) or leaves the
 * current (Sequential) backend in place to stay serial.
 *
 * Prefer RunSafeFilterParallel() at call sites: it additionally skips scoping
 * entirely when already inside another SMP parallel scope (re-entrancy guard),
 * which avoids mutating the process-global SMP singleton mid-parallel-region.
 */
VTKCOMMONCORE_EXPORT vtkSMPTools::Config GetSafeFilterThreadingConfig();

/**
 * Run @p body (a callable wrapping the filter's vtkSMPTools::For region) under
 * the cvista default-threading policy. If we are already inside a parallel scope,
 * @p body runs directly with no singleton mutation (inherits the caller's
 * backend); otherwise it runs inside a vtkSMPTools::LocalScope with the Config
 * from GetSafeFilterThreadingConfig().
 */
template <typename Body>
inline void RunSafeFilterParallel(Body&& body)
{
  // Re-entrancy guard: never touch the (process-global, not-thread-safe) SMP
  // singleton while another thread may be mid-For. Inherit the caller's scope.
  if (vtkSMPTools::IsParallelScope())
  {
    body();
    return;
  }
  vtkSMPTools::LocalScope(GetSafeFilterThreadingConfig(), std::forward<Body>(body));
}

/**
 * True when the opt-in NON-EXACT fast mode is enabled (env CVISTA_FAST, set by the
 * Python cvista.EnableFast()). Default OFF. Read live so it can be toggled at
 * runtime. Filters whose threaded path is not byte-exact gate on this.
 */
VTKCOMMONCORE_EXPORT bool FastModeEnabled();

/**
 * Like RunSafeFilterParallel(), but ONLY threads when FastModeEnabled(). When
 * fast mode is off (the default), @p body runs serially so the filter stays
 * byte-exact vs stock. Use this -- not RunSafeFilterParallel() -- for parallel
 * regions whose output is NOT byte-exact (e.g. order-relaxed topology emission
 * whose cell order depends on thread scheduling).
 */
template <typename Body>
inline void RunFastFilterParallel(Body&& body)
{
  if (!FastModeEnabled())
  {
    body();
    return;
  }
  RunSafeFilterParallel(std::forward<Body>(body));
}

VTK_ABI_NAMESPACE_END
} // namespace cvista

#endif // vtkCVISTASMPDefaults_h
// VTK-HeaderTest-Exclude: vtkCVISTASMPDefaults.h
