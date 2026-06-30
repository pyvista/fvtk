// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkCVISTASMPDefaults.h"

#include "SMP/Common/vtkSMPToolsAPI.h" // For the global SMP singleton state

#include <cstdlib> // For std::getenv
#include <cstring> // For std::strcmp
#include <string>

namespace cvista
{
VTK_ABI_NAMESPACE_BEGIN

namespace
{
// True if the CVISTA_SMP_DEFAULT env var asks us to turn the default off.
bool DefaultThreadingDisabledByEnv()
{
  const char* v = std::getenv("CVISTA_SMP_DEFAULT");
  if (!v)
  {
    return false;
  }
  return std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0 ||
    std::strcmp(v, "OFF") == 0 || std::strcmp(v, "serial") == 0 ||
    std::strcmp(v, "Serial") == 0 || std::strcmp(v, "false") == 0;
}
}

//------------------------------------------------------------------------------
// Opt-in NON-EXACT fast mode. Default OFF: filters whose threaded path is not
// byte-exact (e.g. order-relaxed topology emission) stay serial unless the user
// opts in. Read live from the CVISTA_FAST env var (which cvista.EnableFast() sets),
// so it can be toggled at runtime. Truthy: 1/on/true/yes (any case).
bool FastModeEnabled()
{
  const char* v = std::getenv("CVISTA_FAST");
  if (!v || v[0] == '\0')
  {
    return false;
  }
  return std::strcmp(v, "1") == 0 || std::strcmp(v, "on") == 0 ||
    std::strcmp(v, "ON") == 0 || std::strcmp(v, "On") == 0 ||
    std::strcmp(v, "true") == 0 || std::strcmp(v, "True") == 0 ||
    std::strcmp(v, "TRUE") == 0 || std::strcmp(v, "yes") == 0 ||
    std::strcmp(v, "YES") == 0;
}

//------------------------------------------------------------------------------
// Precedence (first match wins):
//   1. opt-out env CVISTA_SMP_DEFAULT=0/off/serial  -> stay Sequential (serial).
//   2. global backend already != Sequential (user SetBackend / VTK_SMP_BACKEND_IN_USE)
//                                                 -> inherit the global (no change).
//   3. user called Initialize(n) (DesiredNumberOfThread != 0), global still Seq
//                                                 -> STDThread @ n.
//   4. VTK_SMP_MAX_THREADS env set, global still Seq
//                                  -> STDThread, MaxNumberOfThreads=0 so the
//                                     backend reads & honors the env (=1 -> 1).
//   5. default                     -> STDThread capped at CVISTA_SMP_DEFAULT_THREADS.
//
// Note: the IsParallelScope() re-entrancy guard lives in RunSafeFilterParallel()
// (header), so this function never needs to consider it.
vtkSMPTools::Config GetSafeFilterThreadingConfig()
{
  auto& api = vtk::detail::smp::vtkSMPToolsAPI::GetInstance();

  // (1) explicit opt-out -> force serial by keeping Sequential.
  if (DefaultThreadingDisabledByEnv())
  {
    return vtkSMPTools::Config{ std::string("Sequential") };
  }

  // (2) user already chose a non-Sequential backend for the whole library
  //     (e.g. SetBackend("STDThread") or VTK_SMP_BACKEND_IN_USE). Inherit it
  //     verbatim (the Config(api) ctor copies backend + desired-threads +
  //     nested), so we do NOT re-cap to 4 and do NOT change anything.
  const std::string globalBackend = api.GetBackend() ? api.GetBackend() : "Sequential";
  if (globalBackend != "Sequential")
  {
    return vtkSMPTools::Config{ api };
  }

  // (3) explicit programmatic thread count via Initialize(n), backend still Seq.
  const int desired = api.GetInternalDesiredNumberOfThread();
  if (desired != 0)
  {
    return vtkSMPTools::Config{ desired, std::string("STDThread"), false };
  }

  // (4) VTK_SMP_MAX_THREADS env, backend still Seq: switch to STDThread but pass
  //     MaxNumberOfThreads=0 so STDThread::Initialize reads the env itself and
  //     honors the exact value (8 -> 8, 1 -> 1 i.e. effectively serial).
  if (std::getenv("VTK_SMP_MAX_THREADS"))
  {
    return vtkSMPTools::Config{ 0, std::string("STDThread"), false };
  }

  // (5) out-of-the-box default: STDThread capped at 4.
  return vtkSMPTools::Config{ CVISTA_SMP_DEFAULT_THREADS, std::string("STDThread"), false };
}

VTK_ABI_NAMESPACE_END
} // namespace cvista
